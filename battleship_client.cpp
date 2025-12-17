#include "battleship.h"
#include "battleship_windows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <iostream>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <string>
#include <atomic>
#include <chrono>
#include <signal.h>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

// Globals
static int sockfd = -1;

/* Client local state */
struct client_info my_client;
struct client_info enemy_client;
struct game_session current_session;
static int game_started = 0;
static int my_turn = 0;

/* Threads + queues */
static std::thread g_stdin_thread;
static std::mutex g_stdin_mutex;
static std::condition_variable g_stdin_cv;
static std::queue<std::string> g_stdin_queue;
static std::atomic<bool> g_stdin_running{false};

static std::thread g_socket_thread;
static std::mutex g_packet_mutex;
static std::condition_variable g_packet_cv;
static std::queue<packet_t> g_packet_queue;
static std::atomic<bool> g_socket_running{false};

/* Forward declarations of UI functions */
void client_session_selection_ui();
void client_place_ships_ui();
void client_manual_placement_ui();
void client_shooting_ui(struct game_session* session, int player_number);
void handle_packet(const packet_t *p);

/* --- Utility: send/recv packets --- */

static int send_packet_fd(int fd, const packet_t *p) {
    const char *buf = (const char*)p;
    int to_send = PACKET_SIZE;
    int sent = 0;
    while (sent < to_send) {
        int n = send(fd, buf + sent, to_send - sent, 0);
        if (n <= 0) {
            if (n == -1 && errno == EINTR) continue;
            return -1;
        }
        sent += n;
    }
    return sent;
}

static int recv_packet_fd(int fd, packet_t *p) {
    char *buf = (char*)p;
    int to_read = PACKET_SIZE;
    int got = 0;
    while (got < to_read) {
        int n = recv(fd, buf + got, to_read - got, 0);
        if (n <= 0) {
            if (n == -1 && errno == EINTR) continue;
            return -1;
        }
        got += n;
    }
    return got;
}

static void client_send_command(const char *command, const char *arg1, const char *arg2) {
    packet_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    if (command) strncpy(pkt.command, command, PACKET_COMMAND_SIZE - 1);
    if (arg1) strncpy(pkt.arg1, arg1, PACKET_ARG_SIZE_1 - 1);
    if (arg2) strncpy(pkt.arg2, arg2, PACKET_ARG_SIZE_2 - 1);
    send_packet_fd(sockfd, &pkt);
}

/* --- stdin thread --- */

static void stdin_thread_func() {
    std::string line;
    // Use std::getline on std::cin; works for PowerShell/Command Prompt/Windows Terminal and POSIX terminals.
    while (g_stdin_running.load()) {
        if (!std::getline(std::cin, line)) {
            // EOF or error — stop running
            g_stdin_running.store(false);
            g_stdin_cv.notify_all();
            break;
        }
        {
            std::lock_guard<std::mutex> lk(g_stdin_mutex);
            g_stdin_queue.push(line);
        }
        g_stdin_cv.notify_one();
    }
}

static void start_stdin_thread() {
    bool expected = false;
    if (!g_stdin_running.compare_exchange_strong(expected, true)) return;
    g_stdin_thread = std::thread(stdin_thread_func);
}

static void stop_stdin_thread() {
    if (!g_stdin_running.load()) return;
    g_stdin_running.store(false);
    g_stdin_cv.notify_all();
    if (g_stdin_thread.joinable()) g_stdin_thread.join();
}

/* --- socket receiver thread --- */

static void socket_thread_func() {
    packet_t pkt;
    while (g_socket_running.load()) {
        memset(&pkt, 0, sizeof(pkt));
        int got = recv_packet_fd(sockfd, &pkt);
        if (got <= 0) {
            // signal main thread about disconnect by pushing a special packet if desired
            g_socket_running.store(false);
            // notify main thread
            g_packet_cv.notify_all();
            break;
        }
        {
            std::lock_guard<std::mutex> lk(g_packet_mutex);
            g_packet_queue.push(pkt);
        }
        g_packet_cv.notify_one();
    }
}

static void start_socket_thread() {
    bool expected = false;
    if (!g_socket_running.compare_exchange_strong(expected, true)) return;
    g_socket_thread = std::thread(socket_thread_func);
}

static void stop_socket_thread() {
    if (!g_socket_running.load()) return;
    g_socket_running.store(false);
    if (g_socket_thread.joinable()) g_socket_thread.join();
}

/* --- Combined waiting/reading: read_line_with_packet_processing
   Blocks until a full stdin line is available. While waiting, processes
   incoming packets from the socket queue by calling handle_packet().
   Returns 1 if line stored into out, 0 on socket closed/error.
*/
static int read_line_with_packet_processing(char *out, size_t out_size) {
    // Ensure stdin thread is started
    if (!g_stdin_running.load()) start_stdin_thread();

    while (true) {
        // First, process all queued packets (so UI is updated)
        while (true) {
            packet_t pkt;
            bool have = false;
            {
                std::lock_guard<std::mutex> lk(g_packet_mutex);
                if (!g_packet_queue.empty()) {
                    pkt = g_packet_queue.front();
                    g_packet_queue.pop();
                    have = true;
                }
            }
            if (!have) break;
            handle_packet(&pkt);
        }

        // Then, check stdin queue for a line
        {
            std::lock_guard<std::mutex> lk(g_stdin_mutex);
            if (!g_stdin_queue.empty()) {
                std::string s = std::move(g_stdin_queue.front());
                g_stdin_queue.pop();
                if (out && out_size > 0) {
                    strncpy(out, s.c_str(), out_size - 1);
                    out[out_size - 1] = '\0';
                }
                return 1;
            }
        }

        // If socket thread stopped (disconnect), return 0
        if (!g_socket_running.load()) {
            return 0;
        }

        // Wait for either stdin or packet notifications (or timeout to loop)
        std::unique_lock<std::mutex> lk1(g_packet_mutex);
        g_packet_cv.wait_for(lk1, std::chrono::milliseconds(200));
        // loop: will process packets and check stdin again
    }
}

/* --- UI functions --- */

/* Session selection: prints prompt and waits for user line */
void client_session_selection_ui() {
    printf("\n=== SESSION SELECTION ===\n");
    printf("Session list updates automatically. Please type the session number you want to join, or 'auto' for automatic assignment: ");
    fflush(stdout);

    char input[128];
    while (1) {
        int ok = read_line_with_packet_processing(input, sizeof(input));
        if (!ok) {
            printf("\nDisconnected from server while selecting session.\n");
            exit(0);
        }
        // ignore empty lines
        if (strlen(input) == 0) {
            printf("Type session number or 'auto': ");
            fflush(stdout);
            continue;
        }
        if (strcmp(input, "auto") == 0) {
            client_send_command("JOIN_SESSION", "-1", NULL);
            return;
        } else {
            int session_num = atoi(input);
            if (session_num >= 0 && session_num < MAX_SESSIONS) {
                char tmp[32];
                snprintf(tmp, sizeof(tmp), "%d", session_num);
                client_send_command("JOIN_SESSION", tmp, NULL);
                return;
            } else {
                printf("Invalid session number. Please enter 0-%d or 'auto': ", MAX_SESSIONS - 1);
                fflush(stdout);
            }
        }
    }
}

/* Placement choice: automatic or manual */
void client_place_ships_ui() {
    printf("\n=== SHIP PLACEMENT ===\n");
    printf("Choose placement method:\n");
    printf("1. Automatic placement\n");
    printf("2. Manual placement\n");

    char input[64];
    while (1) {
        printf("Your choice (1/2): ");
        fflush(stdout);
        int ok = read_line_with_packet_processing(input, sizeof(input));
        if (!ok) {
            printf("\nDisconnected from server while choosing placement.\n");
            exit(0);
        }
        if (strcmp(input, "1") == 0) {
            client_send_command("PLACEMENT_CHOICE", "auto", NULL);
            return;
        } else if (strcmp(input, "2") == 0) {
            client_send_command("PLACEMENT_CHOICE", "manual", NULL);
            // server should send MANUAL_PLACEMENT or PLACEMENT_START -> MANUAL_PLACEMENT
            return;
        } else {
            printf("Invalid choice. Please type 1 or 2.\n");
        }
    }
}

/* Manual ship placement UI.
   Uses existing helpers: create_game_field, initialize_ships, place_ship_manual, print_field.
*/
// Замените текущую реализацию client_manual_placement_ui на эту (вставьте в battleship_client.cpp)
static int portable_strcasecmp(const char *a, const char *b) {
#ifdef _WIN32
    return _stricmp(a, b);
#else
    return strcasecmp(a, b);
#endif
}

void client_manual_placement_ui() {
    char field[12][12];
    struct ships ship_data;
    int ships_placed[4] = {0,0,0,0};
    char input[128];

    create_game_field(field);
    initialize_ships(&ship_data);

    auto remaining_counts = [&]() {
        int r4 = (1 - ships_placed[0]) > 0 ? (1 - ships_placed[0]) : 0;
        int r3 = (2 - ships_placed[1]) > 0 ? (2 - ships_placed[1]) : 0;
        int r2 = (3 - ships_placed[2]) > 0 ? (3 - ships_placed[2]) : 0;
        int r1 = (4 - ships_placed[3]) > 0 ? (4 - ships_placed[3]) : 0;
        return std::tuple<int,int,int,int>(r4,r3,r2,r1);
    };

    auto total_remaining = [&]() {
        auto [r4,r3,r2,r1] = remaining_counts();
        return r4 + r3 + r2 + r1;
    };

    auto trim = [](char *s) {
        int i = 0;
        while (s[i] && isspace((unsigned char)s[i])) i++;
        if (i) memmove(s, s + i, strlen(s + i) + 1);
        int len = (int)strlen(s);
        while (len > 0 && isspace((unsigned char)s[len-1])) { s[len-1] = '\0'; len--; }
    };

    auto parse_coord = [&](const char *token, int &r, int &c) -> bool {
        if (!token || !token[0]) return false;
        while (*token && isspace((unsigned char)*token)) token++;
        char letter = toupper((unsigned char)token[0]);
        if (!isalpha((unsigned char)letter)) return false;
        int row = -1;
        if (letter >= 'A' && letter <= 'I') row = letter - 'A' + 1;
        else if (letter == 'J' || letter == 'K') row = 10;
        else row = letter - 'A' + 1;
        const char *p = token + 1;
        while (*p && isspace((unsigned char)*p)) p++;
        if (!isdigit((unsigned char)*p)) return false;
        int col = atoi(p);
        if (row < 1 || col < 1 || row > PLAYABLE_SIZE || col > PLAYABLE_SIZE) return false;
        r = row; c = col;
        return true;
    };

    auto trim_cstr = [](char *t) {
        int i = 0; while (t[i] && isspace((unsigned char)t[i])) i++;
        if (i) memmove(t, t + i, strlen(t + i) + 1);
        int len = (int)strlen(t); while (len>0 && isspace((unsigned char)t[len-1])) { t[len-1]=0; len--; }
    };

    printf("\n=== MANUAL SHIP PLACEMENT ===\n");
    printf("Format: A1-A4 (4-cell), B1-B3 (3-cell), C5-C6 (2-cell), D7 (1-cell)\n");
    printf("Available ships: 1x4-cell, 2x3-cell, 3x2-cell, 4x1-cell\n");
    printf("Type placements one per line. When all ships are placed the client will upload the field.\n\n");

    while (1) {
        print_field(field);
        auto [r4,r3,r2,r1] = remaining_counts();
        printf("\nShips remaining: %d four-cell, %d three-cell, %d two-cell, %d one-cell\n",
               r4, r3, r2, r1);
        printf("Enter ship placement: ");
        fflush(stdout);

        // read and trim input
        if (!read_line_with_packet_processing(input, sizeof(input))) {
            printf("Disconnected or error while placing ships.\n");
            return;
        }
        trim(input);
        if (strlen(input) == 0) continue;

        // Accept "done" or "finish" (case-insensitive)
        if (portable_strcasecmp(input, "done") == 0 || portable_strcasecmp(input, "finish") == 0) {
            if (total_remaining() == 0) {
                char packed[PLAYABLE_SIZE * PLAYABLE_SIZE + 1];
                int idx = 0;
                for (int i = 1; i <= PLAYABLE_SIZE; ++i)
                    for (int j = 1; j <= PLAYABLE_SIZE; ++j)
                        packed[idx++] = '0' + (field[i][j] & 0x07);
                packed[idx] = '\0';
                client_send_command("FIELD_UPLOAD", packed, NULL);
                client_send_command("SHIP_PLACED", "manual", NULL);
                return;
            } else {
                printf("Not all ships placed yet (%d remaining).\n", total_remaining());
                continue;
            }
        }

        // parse tokens
        char s[128];
        strncpy(s, input, sizeof(s)-1); s[sizeof(s)-1]=0;
        char *dash = strchr(s, '-');
        char a_tok[64], b_tok[64];
        if (dash) {
            *dash = '\0';
            strncpy(a_tok, s, sizeof(a_tok)-1); a_tok[sizeof(a_tok)-1]=0;
            strncpy(b_tok, dash + 1, sizeof(b_tok)-1); b_tok[sizeof(b_tok)-1]=0;
        } else {
            strncpy(a_tok, s, sizeof(a_tok)-1); a_tok[sizeof(a_tok)-1]=0;
            b_tok[0] = '\0';
        }
        trim_cstr(a_tok); trim_cstr(b_tok);

        int r1c, c1c, r2c, c2c;
        if (!parse_coord(a_tok, r1c, c1c)) {
            printf("Error: invalid coordinate '%s'\n", a_tok);
            continue;
        }
        if (b_tok[0] == '\0') {
            r2c = r1c; c2c = c1c;
        } else {
            if (!parse_coord(b_tok, r2c, c2c)) {
                printf("Error: invalid coordinate '%s'\n", b_tok);
                continue;
            }
        }

        // orientation & length
        if (r1c != r2c && c1c != c2c) {
            printf("Error: ships must be placed horizontally or vertically (no diagonals).\n");
            continue;
        }
        int len = 0;
        int dr = 0, dc = 0;
        if (r1c == r2c) {
            dc = (c2c >= c1c) ? 1 : -1;
            len = abs(c2c - c1c) + 1;
        } else {
            dr = (r2c >= r1c) ? 1 : -1;
            len = abs(r2c - r1c) + 1;
        }
        if (len < 1 || len > 4) {
            printf("Error: invalid ship length %d (must be 1..4).\n", len);
            continue;
        }

        int idx = (len==4)?0:(len==3)?1:(len==2)?2:3;
        int allowed = (idx==0)?(1 - ships_placed[0]) : (idx==1)?(2 - ships_placed[1]) : (idx==2)?(3 - ships_placed[2]) : (4 - ships_placed[3]);
        if (allowed <= 0) {
            printf("Error: no remaining ships of length %d available.\n", len);
            continue;
        }

        // validate placement (bounds, overlap, adjacency)
        bool ok = true;
        int rr = r1c, cc = c1c;
        for (int t = 0; t < len; ++t) {
            if (rr < 1 || rr > PLAYABLE_SIZE || cc < 1 || cc > PLAYABLE_SIZE) { ok = false; break; }
            if (field[rr][cc] != 0) { ok = false; break; }
            for (int ni = rr - 1; ni <= rr + 1; ++ni) {
                for (int nj = cc - 1; nj <= cc + 1; ++nj) {
                    if (ni < 1 || ni > PLAYABLE_SIZE || nj < 1 || nj > PLAYABLE_SIZE) continue;
                    if (field[ni][nj] != 0) { ok = false; break; }
                }
                if (!ok) break;
            }
            if (!ok) break;
            rr += dr; cc += dc;
        }
        if (!ok) {
            printf("Error: placement conflicts with existing ships or adjacency rules.\n");
            continue;
        }

        // place ship
        rr = r1c; cc = c1c;
        for (int t = 0; t < len; ++t) {
            field[rr][cc] = 1;
            rr += dr; cc += dc;
        }
        ships_placed[idx] += 1;
        printf("Ship placed successfully!\n");

        if (total_remaining() == 0) {
            printf("All ships placed successfully!\n");
            print_field(field);
            char packed[PLAYABLE_SIZE * PLAYABLE_SIZE + 1];
            int id = 0;
            for (int i = 1; i <= PLAYABLE_SIZE; ++i) {
                for (int j = 1; j <= PLAYABLE_SIZE; ++j) {
                    char ch = '0' + (field[i][j] & 0x07);
                    packed[id++] = ch;
                }
            }
            packed[id] = '\0';
            client_send_command("FIELD_UPLOAD", packed, NULL);
            client_send_command("SHIP_PLACED", "manual", NULL);
            return;
        }
    }
}

/* Shooting UI: interactive input for shots */
void client_shooting_ui(struct game_session* session, int player_number) {
    if (!my_turn) {
        printf("Wait for your turn!\n");
        return;
    }
    char input[64];
    print_game_session(session, player_number);
    printf("Enter shot coordinates (e.g., A1): ");
    fflush(stdout);

    int ok = read_line_with_packet_processing(input, sizeof(input));
    if (!ok) {
        printf("\nDisconnected from server while waiting for shot input.\n");
        exit(0);
    }
    client_send_command("SHOT", input, NULL);
}

/* --- Packet handling (called only from main thread) --- */
void handle_packet(const packet_t *p) {
    if (!p) return;
    char cmd[PACKET_COMMAND_SIZE];
    char a1[PACKET_ARG_SIZE_1];
    char a2[PACKET_ARG_SIZE_2];
    memset(cmd, 0, sizeof(cmd));
    memset(a1, 0, sizeof(a1));
    memset(a2, 0, sizeof(a2));
    strncpy(cmd, p->command, PACKET_COMMAND_SIZE - 1);
    strncpy(a1, p->arg1, PACKET_ARG_SIZE_1 - 1);
    strncpy(a2, p->arg2, PACKET_ARG_SIZE_2 - 1);

    if (strcmp(cmd, "SESSION_LIST") == 0) {
        printf("\n=== AVAILABLE SESSIONS ===\n%s\n", a1);
        fflush(stdout);
    } else if (strcmp(cmd, "WELCOME") == 0) {
        printf("%s\n", a1);
    } else if (strcmp(cmd, "PLAYER_ASSIGNED") == 0) {
        my_client.player_number = atoi(a1);
        printf("You are Player %d\n", my_client.player_number);
        my_client.session_id = current_session.id;
        my_client.ready = 0;
        create_game_field(my_client.field);
        initialize_ships(&my_client.ship_data);
    } else if (strcmp(cmd, "SESSION_CREATED") == 0) {
        current_session.id = atoi(a1);
        printf("Joined game session %d\n", current_session.id);
    } else if (strcmp(cmd, "LEADERBOARD") == 0) {
        if (strcmp(a1, "EMPTY") == 0) {
            printf("\n=== LEADERBOARD ===\nNo entries yet.\n\n");
        } else {
            printf("\n=== LEADERBOARD ===\n%s\n", a1);
        }
    } else if (strcmp(cmd, "PLACEMENT_START") == 0) {
        printf("Starting ship placement...\n");
        client_place_ships_ui();
    } else if (strcmp(cmd, "MANUAL_PLACEMENT") == 0) {
        printf("Manual placement mode (server requested manual placement)\n");
        client_manual_placement_ui();
    } else if (strcmp(cmd, "GAME_START") == 0) {
        game_started = 1;
        printf("=== GAME STARTED! ===\n");
        create_game_field(enemy_client.field);
        enemy_client.fd = -1;
        enemy_client.session_id = current_session.id;
        enemy_client.player_number = (my_client.player_number == 1) ? 2 : 1;
        enemy_client.ready = 1;
        initialize_ships(&enemy_client.ship_data);

        if (my_client.player_number == 1) {
            current_session.player1 = &my_client;
            current_session.player2 = &enemy_client;
        } else {
            current_session.player2 = &my_client;
            current_session.player1 = &enemy_client;
        }
        current_session.game_started = 1;
        current_session.current_turn = 1;

        printf("Game started! You are Player %d\n", my_client.player_number);
        print_game_session(&current_session, my_client.player_number);
    } else if (strcmp(cmd, "FIELD_UPDATE") == 0) {
        if (strlen(a1) >= PLAYABLE_SIZE * PLAYABLE_SIZE) {
            int idx = 0;
            for (int i = 1; i <= PLAYABLE_SIZE; i++) {
                for (int j = 1; j <= PLAYABLE_SIZE; j++) {
                    char c = a1[idx++];
                    my_client.field[i][j] = (c >= '0' && c <= '3') ? (c - '0') : 0;
                }
            }
        }
    } else if (strcmp(cmd, "ENEMY_FOG_UPDATE") == 0) {
        if (strlen(a1) >= PLAYABLE_SIZE * PLAYABLE_SIZE) {
            int idx = 0;
            for (int i = 1; i <= PLAYABLE_SIZE; i++) {
                for (int j = 1; j <= PLAYABLE_SIZE; j++) {
                    char c = a1[idx++];
                    enemy_client.field[i][j] = (c >= '0' && c <= '3') ? (c - '0') : 0;
                }
            }
        }
    } else if (strcmp(cmd, "YOUR_TURN") == 0) {
        my_turn = 1;
        current_session.current_turn = my_client.player_number;
        printf("\n=== YOUR TURN ===\n");
        client_shooting_ui(&current_session, my_client.player_number);
    } else if (strcmp(cmd, "OPPONENT_TURN") == 0) {
        my_turn = 0;
        current_session.current_turn = (my_client.player_number == 1) ? 2 : 1;
        printf("\n=== OPPONENT'S TURN ===\nWaiting for opponent's move...\n");
    } else if (strcmp(cmd, "SHOT_RESULT") == 0) {
        printf("Your shot result: %s -> %s\n", a1, a2);
        if (strcmp(a2, "MISS") == 0) {
            my_turn = 0;
            current_session.current_turn = (my_client.player_number == 1) ? 2 : 1;
        }
        print_game_session(&current_session, my_client.player_number);
    } else if (strcmp(cmd, "OPPONENT_SHOT") == 0) {
        printf("Opponent shot at: %s -> %s\n", a1, a2);
        if (strlen(a1) > 0) {
            int r = simple_shot(my_client.field, &my_client.ship_data, a1);
            if (r == 1) {
                printf("Opponent hit your ship!\n");
                print_game_session(&current_session, my_client.player_number);
            } else if (r == 0) {
                printf("Opponent missed!\n");
            }
        }
    } else if (strcmp(cmd, "OPPONENT_DISCONNECTED") == 0) {
        printf("Opponent disconnected. You win!\n");
        exit(0);
    } else if (strcmp(cmd, "GAME_OVER") == 0) {
        printf("\n=== GAME OVER ===\n");
        if (strcmp(a1, "WIN") == 0) printf("You won!\n");
        else if (strcmp(a1, "LOSE") == 0) printf("You lost.\n");
        else printf("Game ended: %s\n", a1);
        stop_stdin_thread();
        stop_socket_thread();
        sock_close(sockfd);
        exit(0);
    } else if (strcmp(cmd, "ERROR") == 0) {
        printf("Server error: %s\n", a1);
    } else {
        // Unknown command: print raw
        printf("SERVER: %s %s %s\n", cmd, a1, a2);
    }
}

/* --- Socket connect and threads management --- */

static void socket_cleanup_and_exit() {
    stop_stdin_thread();
    stop_socket_thread();
    if (sockfd != -1) sock_close(sockfd);
#ifdef _WIN32
    cleanup_winsock();
#endif
}

/* Signal handlers to gracefully stop threads on Ctrl+C */
static void on_sigint(int) {
    printf("\nSIGINT received, exiting...\n");
    socket_cleanup_and_exit();
    exit(0);
}

/* Main */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        printf("Usage: %s <host> <port> [nick]\n", argv[0]);
        return 1;
    }
    const char *host = argv[1];
    int port = atoi(argv[2]);
    std::string nick;
    if (argc >= 4) {
        nick = argv[3];
    } else {
        // ask for nickname interactively before starting threads
        printf("Enter your nickname: ");
        fflush(stdout);
        if (!std::getline(std::cin, nick)) {
            fprintf(stderr, "Failed to read nickname\n");
            return 1;
        }
        if (nick.size() == 0) nick = "player";
    }

#ifdef _WIN32
    if (!init_winsock()) {
        fprintf(stderr, "Failed to initialize Winsock\n");
        return 1;
    }
#endif

    // Setup initial client state
    my_client.fd = -1;
    my_client.session_id = -1;
    my_client.player_number = 0;
    my_client.ready = 0;
    enemy_client.fd = -1;
    enemy_client.session_id = -1;
    enemy_client.player_number = 0;
    enemy_client.ready = 0;
    create_game_field(enemy_client.field);
    initialize_ships(&enemy_client.ship_data);

    current_session.id = -1;
    current_session.player1 = NULL;
    current_session.player2 = NULL;
    current_session.game_started = 0;
    current_session.current_turn = 1;

    // Setup signal handler
    signal(SIGINT, on_sigint);

    // Create socket and connect
    struct sockaddr_in server;
    struct hostent *hp;

    sockfd = (int)socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == -1) {
        perror("socket");
        return 1;
    }
    server.sin_family = AF_INET;
    hp = gethostbyname(host);
    if (!hp) {
        fprintf(stderr, "Unknown host: %s\n", host);
        sock_close(sockfd);
        return 1;
    }
    memcpy(&server.sin_addr, hp->h_addr_list[0], hp->h_length);
    server.sin_port = htons(port);

    printf("Connecting to server %s:%d...\n", host, port);
    if (connect(sockfd, (struct sockaddr*)&server, sizeof(server)) == -1) {
        perror("connect");
        sock_close(sockfd);
        return 1;
    }
    printf("Connected.\n");

    // Start stdin and socket threads
    start_stdin_thread();
    start_socket_thread();

    // Send nickname
    client_send_command("SET_NICK", nick.c_str(), NULL);

    // IMPORTANT: prompt user to select session immediately after setting nick.
    // This ensures that when SESSION_LIST arrives (printed by handle_packet),
    // the client is waiting for input and will send JOIN_SESSION on user's choice.
    client_session_selection_ui();

    // After the user has joined a session, main loop processes incoming packets normally.
    while (true) {
        // Wait for a packet or socket termination
        std::unique_lock<std::mutex> lk(g_packet_mutex);
        g_packet_cv.wait(lk, [] { return !g_packet_queue.empty() || !g_socket_running.load(); });
        // Process all queued packets
        while (!g_packet_queue.empty()) {
            packet_t pkt = g_packet_queue.front();
            g_packet_queue.pop();
            lk.unlock();
            handle_packet(&pkt);
            lk.lock();
        }
        if (!g_socket_running.load()) {
            printf("Socket thread has stopped (server disconnected?). Exiting.\n");
            break;
        }
    }

    socket_cleanup_and_exit();
    return 0;
}

