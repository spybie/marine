#include "battleship.h"
#include "battleship_windows.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <syslog.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#endif

#define BUFFER_SIZE 1024

#ifdef _WIN32
#define PID_FILE "battleship_server.pid"
#else
#define PID_FILE "/tmp/battleship_server.pid"
#endif

struct client_info clients[MAX_CLIENTS];
struct game_session sessions[MAX_SESSIONS];
static int g_server_sock = -1;

/* Forward declarations */
void send_session_list(struct client_info* client);
int auto_assign_to_session(struct client_info* client);
int assign_to_session(struct client_info* client, int session_id);
void broadcast_session_list();
void disconnect_client(struct client_info* c);

/* PID file helpers */
static int write_pid_file() {
    FILE* pid_file = fopen(PID_FILE, "w");
    if (!pid_file) {
        fprintf(stderr, "Failed to create PID file '%s': %s\n", PID_FILE, strerror(errno));
        return -1;
    }
#ifdef _WIN32
    fprintf(pid_file, "%u\n", (unsigned)GetCurrentProcessId());
#else
    fprintf(pid_file, "%d\n", getpid());
#endif
    fclose(pid_file);
    return 0;
}

static void remove_pid_file() {
    remove(PID_FILE);
}

/* Daemonize (POSIX only) */
static int daemonize() {
#ifdef _WIN32
    /* Not daemonizing on Windows */
    return 0;
#else
    pid_t pid = fork();
    if (pid < 0) {
        perror("Failed to fork");
        return -1;
    }
    if (pid > 0) exit(0);
    umask(0);
    if (setsid() < 0) { perror("setsid"); return -1; }
    if (chdir("/") < 0) { perror("chdir"); return -1; }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_WRONLY);
    return 0;
#endif
}

/* Packet helpers */
static int send_packet_fd(int fd, const packet_t* p) {
    const char* buf = (const char*)p;
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

static int recv_packet_fd(int fd, packet_t* p) {
    char* buf = (char*)p;
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

static void send_packet_by_parts(int fd, const char* command, const char* arg1, const char* arg2) {
    packet_t p;
    memset(&p, 0, sizeof(p));
    if (command) strncpy(p.command, command, PACKET_COMMAND_SIZE - 1);
    if (arg1) strncpy(p.arg1, arg1, PACKET_ARG_SIZE_1 - 1);
    if (arg2) strncpy(p.arg2, arg2, PACKET_ARG_SIZE_2 - 1);
    send_packet_fd(fd, &p);
}

void send_message(int fd, const char* message) {
    send_packet_by_parts(fd, "RAW", message, NULL);
}

/* Field sync helpers */
void send_full_field_update(struct client_info* client) {
    if (!client || client->fd == -1) return;
    char payload[PLAYABLE_SIZE * PLAYABLE_SIZE + 1];
    int idx = 0;
    for (int i = 1; i <= PLAYABLE_SIZE; i++) {
        for (int j = 1; j <= PLAYABLE_SIZE; j++) {
            payload[idx++] = '0' + (client->field[i][j] & 0x07);
        }
    }
    payload[idx] = '\0';
    send_packet_by_parts(client->fd, "FIELD_UPDATE", payload, NULL);
}

void build_fog_field(char dest[12][12], char src[12][12]) {
    for (int i = 1; i <= PLAYABLE_SIZE; i++) {
        for (int j = 1; j <= PLAYABLE_SIZE; j++) {
            char v = src[i][j];
            if (v == 2 || v == 3) dest[i][j] = v;
            else dest[i][j] = 0;
        }
    }
}

void send_fog_update(struct client_info* attacker, struct client_info* defender) {
    if (!attacker || attacker->fd == -1 || !defender) return;
    char fog[12][12];
    build_fog_field(fog, defender->field);
    char payload[PLAYABLE_SIZE * PLAYABLE_SIZE + 1];
    int idx = 0;
    for (int i = 1; i <= PLAYABLE_SIZE; i++) {
        for (int j = 1; j <= PLAYABLE_SIZE; j++) {
            payload[idx++] = '0' + (fog[i][j] & 0x07);
        }
    }
    payload[idx] = '\0';
    send_packet_by_parts(attacker->fd, "ENEMY_FOG_UPDATE", payload, NULL);
}

/* Game/session helpers */
void handle_ship_placement(struct client_info* client) {
    if (!client) return;
    if (client->player_number == 1) {
        send_packet_by_parts(client->fd, "PLACEMENT_START", "1", NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 && clients[i].session_id == client->session_id && clients[i].player_number == 2) {
                send_packet_by_parts(clients[i].fd, "WAIT", "Opponent is placing ships", NULL);
            }
        }
    } else {
        send_packet_by_parts(client->fd, "PLACEMENT_START", "2", NULL);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd != -1 && clients[i].session_id == client->session_id && clients[i].player_number == 1) {
                send_packet_by_parts(clients[i].fd, "WAIT", "Opponent is placing ships", NULL);
            }
        }
    }
}

void start_game_session(int session_id) {
    if (session_id < 0 || session_id >= MAX_SESSIONS) return;
    struct game_session* sess = &sessions[session_id];
    struct client_info* player1 = sess->player1;
    struct client_info* player2 = sess->player2;

    if (player1 && player2 && player1->ready && player2->ready) {
        sess->game_started = 1;
        sess->current_turn = 1;
        
        send_packet_by_parts(player1->fd, "GAME_START", "1", NULL);
        send_packet_by_parts(player2->fd, "GAME_START", "2", NULL);

        send_full_field_update(player1);
        send_full_field_update(player2);

        send_fog_update(player1, player2);
        send_fog_update(player2, player1);

        send_packet_by_parts(player1->fd, "YOUR_TURN", NULL, NULL);
        send_packet_by_parts(player2->fd, "OPPONENT_TURN", NULL, NULL);

        printf("Game session %d started! Player1: %s, Player2: %s\n", 
               session_id, player1->nickname, player2->nickname);
    } else {
        printf("Cannot start game session %d - players not ready\n", session_id);
    }
}

/* Centralized start check to avoid races */
void try_start_session(int session_id) {
    if (session_id < 0 || session_id >= MAX_SESSIONS) return;
    struct game_session* sess = &sessions[session_id];
    if (!sess) return;
    if (sess->game_started) return;

    struct client_info* p1 = sess->player1;
    struct client_info* p2 = sess->player2;
    if (!p1 || !p2) return;
    if (p1->fd == -1 || p2->fd == -1) return;
    if (!p1->ready || !p2->ready) return;

    printf("Both players ready for session %d, starting game (try_start_session)\n", session_id);
    start_game_session(session_id);
}

/* Process incoming packets from client */
void process_client_packet(struct client_info* client, const packet_t* p) {
    if (!client || !p) return;

    char command[PACKET_COMMAND_SIZE];
    char arg1[PACKET_ARG_SIZE_1];
    char arg2[PACKET_ARG_SIZE_2];
    memset(command, 0, sizeof(command));
    memset(arg1, 0, sizeof(arg1));
    memset(arg2, 0, sizeof(arg2));
    strncpy(command, p->command, PACKET_COMMAND_SIZE - 1);
    strncpy(arg1, p->arg1, PACKET_ARG_SIZE_1 - 1);
    strncpy(arg2, p->arg2, PACKET_ARG_SIZE_2 - 1);

    if (strcmp(command, "SET_NICK") == 0) {
        strncpy(client->nickname, arg1, sizeof(client->nickname) - 1);
        client->nickname[sizeof(client->nickname) - 1] = 0;
        send_session_list(client);
        return;
    }
    
    if (strcmp(command, "JOIN_SESSION") == 0) {
        int session_id = atoi(arg1);
        
        if ((session_id < 0 && session_id != -1) || session_id >= MAX_SESSIONS) {
            send_packet_by_parts(client->fd, "ERROR", "Invalid session number", NULL);
            send_session_list(client);
            return;
        }
        
        int result;
        if (session_id == -1) {
            result = auto_assign_to_session(client);
        } else {
            result = assign_to_session(client, session_id);
        }
        
        if (result == -1) {
            send_packet_by_parts(client->fd, "ERROR", "Session is full or unavailable", NULL);
            send_session_list(client);
        } else {
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "%d", result);
            send_packet_by_parts(client->fd, "SESSION_CREATED", tmp, NULL);
            snprintf(tmp, sizeof(tmp), "%d", client->player_number);
            send_packet_by_parts(client->fd, "PLAYER_ASSIGNED", tmp, NULL);
            
            printf("Client %s joined session %d as player %d\n", 
                   client->nickname, result, client->player_number);
            
            if (client->player_number == 1) {
                send_packet_by_parts(client->fd, "WAIT", "Waiting for opponent...", NULL);
            } else {
                send_packet_by_parts(client->fd, "WELCOME", "Game starting soon!", NULL);
                if (sessions[result].player1) {
                    handle_ship_placement(sessions[result].player1);
                }
            }
            broadcast_session_list();
        }
        return;
    }

    if (strcmp(command, "FIELD_UPLOAD") == 0) {
        int len = strlen(arg1);
        if (len >= PLAYABLE_SIZE * PLAYABLE_SIZE) {
            int idx = 0;
            for (int i = 1; i <= PLAYABLE_SIZE; i++) {
                for (int j = 1; j <= PLAYABLE_SIZE; j++) {
                    char c = arg1[idx++];
                    client->field[i][j] = (c >= '0' && c <= '3') ? (c - '0') : 0;
                }
            }
            client->ready = 1;
            send_full_field_update(client);
            send_packet_by_parts(client->fd, "PLACEMENT_DONE", NULL, NULL);

            int session_id = client->session_id;
            if (session_id >= 0 && session_id < MAX_SESSIONS) {
                struct game_session* sess = &sessions[session_id];
                struct client_info* other_player = (client->player_number == 1) ? sess->player2 : sess->player1;
                
                if (other_player && other_player->ready) {
                    printf("Both players ready after manual placement, starting game...\n");
                    start_game_session(session_id);
                } else {
                    if (client->player_number == 1) {
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (clients[i].fd != -1 && clients[i].session_id == session_id && clients[i].player_number == 2) {
                                handle_ship_placement(&clients[i]);
                                break;
                            }
                        }
                    }
                }
            }
            try_start_session(client->session_id);
        } else {
            send_packet_by_parts(client->fd, "ERROR", "FIELD_UPLOAD_INVALID", NULL);
        }
        return;
    }

    if (strcmp(command, "PLACEMENT_CHOICE") == 0) {
        if (strcmp(arg1, "auto") == 0) {
            create_game_field(client->field);
            initialize_ships(&client->ship_data);
            place_ships(client->field, &client->ship_data);

            send_full_field_update(client);

            client->ready = 1;
            send_packet_by_parts(client->fd, "PLACEMENT_DONE", NULL, NULL);
            
            printf("Player %d in session %d finished auto placement\n", 
                   client->player_number, client->session_id);

            int session_id = client->session_id;
            if (session_id >= 0 && session_id < MAX_SESSIONS) {
                struct game_session* sess = &sessions[session_id];
                struct client_info* other_player = (client->player_number == 1) ? sess->player2 : sess->player1;
                
                if (other_player && other_player->ready) {
                    printf("Both players ready after auto placement, starting game...\n");
                    start_game_session(session_id);
                } else {
                    if (client->player_number == 1) {
                        printf("Starting placement for player 2 after player 1 auto\n");
                        for (int i = 0; i < MAX_CLIENTS; i++) {
                            if (clients[i].fd != -1 && clients[i].session_id == session_id && clients[i].player_number == 2) {
                                handle_ship_placement(&clients[i]);
                                break;
                            }
                        }
                    }
                }
            }
            try_start_session(client->session_id);
            return;
        } else if (strcmp(arg1, "manual") == 0) {
            send_packet_by_parts(client->fd, "MANUAL_PLACEMENT", NULL, NULL);
            return;
        }
    } else if (strcmp(command, "SHIP_PLACED") == 0) {
        client->ready = 1;

        send_full_field_update(client);
        send_packet_by_parts(client->fd, "PLACEMENT_DONE", NULL, NULL);

        int session_id = client->session_id;
        if (session_id >= 0 && session_id < MAX_SESSIONS) {
            struct game_session* sess = &sessions[session_id];
            struct client_info* other_player = (client->player_number == 1) ? sess->player2 : sess->player1;
            
            if (other_player && other_player->ready) {
                printf("Both players ready after manual placement, starting game...\n");
                start_game_session(session_id);
            } else {
                if (client->player_number == 1) {
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (clients[i].fd != -1 && clients[i].session_id == session_id && clients[i].player_number == 2) {
                            handle_ship_placement(&clients[i]);
                            break;
                        }
                    }
                }
            }
        }
        try_start_session(session_id);
        return;
    } else if (strcmp(command, "SHOT") == 0) {
        int session_id = client->session_id;
        if (session_id < 0 || session_id >= MAX_SESSIONS) {
            send_packet_by_parts(client->fd, "ERROR", "INVALID_SESSION", NULL);
            return;
        }
        struct game_session* sess = &sessions[session_id];
        if (sess->current_turn != client->player_number) {
            send_packet_by_parts(client->fd, "NOT_YOUR_TURN", NULL, NULL);
            return;
        }
        struct client_info* opponent = (client->player_number == 1) ? sess->player2 : sess->player1;
        if (!opponent) {
            send_packet_by_parts(client->fd, "ERROR", "NO_OPPONENT", NULL);
            return;
        }

        int result = simple_shot(opponent->field, &opponent->ship_data, arg1);

        if (result == 1) {
            send_packet_by_parts(client->fd, "SHOT_RESULT", arg1, "HIT");
            send_packet_by_parts(opponent->fd, "OPPONENT_SHOT", arg1, "HIT");
        } else if (result == 0) {
            send_packet_by_parts(client->fd, "SHOT_RESULT", arg1, "MISS");
            send_packet_by_parts(opponent->fd, "OPPONENT_SHOT", arg1, "MISS");
            sess->current_turn = 3 - client->player_number;
        } else {
            send_packet_by_parts(client->fd, "SHOT_RESULT", arg1, "INVALID");
        }

        if (client->player_number == 1) {
            send_fog_update(sess->player1, sess->player2);
        } else {
            send_fog_update(sess->player2, sess->player1);
        }
        send_full_field_update(opponent);

        if (all_ships_sunk(&opponent->ship_data)) {
            update_leaderboard(client->nickname);

            send_packet_by_parts(client->fd, "GAME_OVER", "WIN", NULL);
            send_packet_by_parts(opponent->fd, "GAME_OVER", "LOSE", NULL);

            sessions[session_id].game_started = 0;
            return;
        }

        if (sess->current_turn == 1) {
            send_packet_by_parts(sess->player1->fd, "YOUR_TURN", NULL, NULL);
            send_packet_by_parts(sess->player2->fd, "OPPONENT_TURN", NULL, NULL);
        } else {
            send_packet_by_parts(sess->player1->fd, "OPPONENT_TURN", NULL, NULL);
            send_packet_by_parts(sess->player2->fd, "YOUR_TURN", NULL, NULL);
        }

        send_full_field_update(sess->player1);
        send_full_field_update(sess->player2);

        return;
    } else if (strcmp(command, "REQUEST_FIELD") == 0) {
        send_full_field_update(client);
        return;
    } else if (strcmp(command, "QUIT") == 0) {
        send_packet_by_parts(client->fd, "BYE", NULL, NULL);
        return;
    } else if (strcmp(command, "DISCONNECT") == 0) {
        send_packet_by_parts(client->fd, "BYE", NULL, NULL);
        disconnect_client(client);
        return;
    }

    send_packet_by_parts(client->fd, "ERROR", "UNKNOWN_COMMAND", NULL);
}

/* Session assignment helpers */
void send_session_list(struct client_info* client) {
    char session_list[PACKET_ARG_SIZE_1] = "";
    char buffer[256];
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].id != -1) {
            int players_count = 0;
            char player1_nick[64] = "Waiting...";
            char player2_nick[64] = "Waiting...";
            
            if (sessions[i].player1) {
                players_count++;
                strncpy(player1_nick, sessions[i].player1->nickname, sizeof(player1_nick) - 1);
            }
            if (sessions[i].player2) {
                players_count++;
                strncpy(player2_nick, sessions[i].player2->nickname, sizeof(player2_nick) - 1);
            }
            
            snprintf(buffer, sizeof(buffer), 
                    "Session %d: %s vs %s (%d/2 players)\n", 
                    i, player1_nick, player2_nick, players_count);
            
            if (strlen(session_list) + strlen(buffer) < PACKET_ARG_SIZE_1 - 1) {
                strcat(session_list, buffer);
            }
        }
    }
    
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].id == -1) {
            snprintf(buffer, sizeof(buffer), "Session %d: EMPTY (0/2 players)\n", i);
            if (strlen(session_list) + strlen(buffer) < PACKET_ARG_SIZE_1 - 1) {
                strcat(session_list, buffer);
            }
        }
    }
    
    if (strlen(session_list) == 0) {
        strcpy(session_list, "No sessions available. New sessions will be created automatically.\n");
    }
    
    send_packet_by_parts(client->fd, "SESSION_LIST", session_list, NULL);
}

int assign_to_session(struct client_info* client, int session_id) {
    if (session_id < 0 || session_id >= MAX_SESSIONS) {
        return -1;
    }
    
    if (sessions[session_id].id != -1) {
        if (sessions[session_id].player1 && sessions[session_id].player2) {
            return -1;
        }
    }
    
    if (sessions[session_id].id == -1) {
        sessions[session_id].id = session_id;
        sessions[session_id].player1 = client;
        sessions[session_id].player2 = NULL;
        sessions[session_id].game_started = 0;
        sessions[session_id].current_turn = 1;
        client->session_id = session_id;
        client->player_number = 1;
    } else {
        if (sessions[session_id].player1 == NULL) {
            sessions[session_id].player1 = client;
            client->session_id = session_id;
            client->player_number = 1;
        } else if (sessions[session_id].player2 == NULL) {
            sessions[session_id].player2 = client;
            client->session_id = session_id;
            client->player_number = 2;
        } else {
            return -1;
        }
    }
    
    return session_id;
}

int auto_assign_to_session(struct client_info* client) {
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (sessions[i].id == -1) {
            return assign_to_session(client, i);
        }
        if (sessions[i].player2 == NULL) {
            return assign_to_session(client, i);
        }
    }
    return -1;
}

void disconnect_client(struct client_info* c) {
    if (!c || c->fd == -1) return;

    int session_id = c->session_id;
    struct client_info* opponent = NULL;

    if (session_id != -1) {
        opponent = (c->player_number == 1)
                 ? sessions[session_id].player2
                 : sessions[session_id].player1;
    }

    if (opponent && opponent->fd != -1) {
        send_packet_by_parts(opponent->fd, "OPPONENT_DISCONNECTED", NULL, NULL);
    }

    printf("Client %d disconnected.\n", c->fd);

    sock_close(c->fd);
    c->fd = -1;

    if (session_id != -1) {
        if (sessions[session_id].player1 == c)
            sessions[session_id].player1 = NULL;
        if (sessions[session_id].player2 == c)
            sessions[session_id].player2 = NULL;

        if (!sessions[session_id].player1 && !sessions[session_id].player2) {
            sessions[session_id].id = -1;
            sessions[session_id].game_started = 0;
            printf("Session %d cleared.\n", session_id);
        }
        broadcast_session_list();
    }
}

void handle_server_sigint(int sig) {
    (void)sig;
    printf("=== SERVER SHUTDOWN INITIATED ===\n");

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1) {
            send_packet_by_parts(clients[i].fd, "GAME_OVER", "SERVER_SHUTDOWN", NULL);
            sock_close(clients[i].fd);
            clients[i].fd = -1;
        }
    }

    if (g_server_sock != -1) {
        sock_close(g_server_sock);
    }

    remove_pid_file();
    printf("Server terminated.\n");
    exit(0);
}

void broadcast_session_list() {
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd != -1 && clients[i].session_id == -1) {
            send_session_list(&clients[i]);
        }
    }
}

void send_leaderboard_to_client(int fd) {
    FILE* f = fopen("leaderboard.txt", "r");
    char line[200];

    if (!f) {
        send_packet_by_parts(fd, "LEADERBOARD", "EMPTY", NULL);
        return;
    }

    char payload[PACKET_ARG_SIZE_1];
    memset(payload, 0, sizeof(payload));

    while (fgets(line, sizeof(line), f)) {
        if (strlen(payload) + strlen(line) + 1 >= PACKET_ARG_SIZE_1)
            break;
        strcat(payload, line);
    }

    fclose(f);
    send_packet_by_parts(fd, "LEADERBOARD", payload, NULL);
}

/* Main server loop */
int main(int argc, char* argv[]) {
    int server_sock;
    socklen_t length;
    struct sockaddr_in server;

#ifdef _WIN32
    if (!init_winsock()) {
        fprintf(stderr, "Failed to initialize Winsock\n");
        return 1;
    }
#endif

    struct pollfd fds[MAX_CLIENTS + 1];
    int nfds = 1;
    int i;
    int daemon_mode = 0;
    int port = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--daemon") == 0) {
            daemon_mode = 1;
        } else if (strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) {
            if (i + 1 < argc) {
                port = atoi(argv[++i]);
            }
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printf("Usage: %s [-d|--daemon] [-p|--port PORT]\n", argv[0]);
#ifdef _WIN32
            cleanup_winsock();
#endif
            return 0;
        }
    }

#ifdef _WIN32
    if (daemon_mode) {
        printf("Daemon mode not supported on Windows; running in foreground.\n");
    }
    daemon_mode = 0;
    signal(SIGINT, handle_server_sigint);
    signal(SIGTERM, handle_server_sigint);
#else
    openlog("battleship_server", LOG_PID | LOG_NDELAY, LOG_DAEMON);

    if (daemon_mode) {
        syslog(LOG_INFO, "Starting battleship server in daemon mode");
        if (daemonize() < 0) {
            syslog(LOG_ERR, "Failed to daemonize");
            return 1;
        }
        signal(SIGINT, handle_server_sigint);
        signal(SIGTERM, handle_server_sigint);
        signal(SIGHUP, SIG_IGN);
        signal(SIGTSTP, SIG_IGN);
        signal(SIGQUIT, SIG_IGN);
    } else {
        signal(SIGINT, handle_server_sigint);
        signal(SIGTSTP, handle_server_sigint);
    }
#endif

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);
#endif

    if (write_pid_file() < 0) {
        fprintf(stderr, "Failed to write PID file\n");
#ifdef _WIN32
        cleanup_winsock();
#endif
        return 1;
    }

    for (i = 0; i < MAX_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].session_id = -1;
        clients[i].ready = 0;
        create_game_field(clients[i].field);
        initialize_ships(&clients[i].ship_data);
    }

    for (i = 0; i < MAX_SESSIONS; i++) {
        sessions[i].id = -1;
        sessions[i].player1 = NULL;
        sessions[i].player2 = NULL;
        sessions[i].game_started = 0;
        sessions[i].current_turn = 1;
    }

    for (i = 0; i < MAX_CLIENTS + 1; i++) {
        fds[i].fd = -1;
        fds[i].events = POLLIN;
        fds[i].revents = 0;
    }

    g_server_sock = socket(AF_INET, SOCK_STREAM, 0);
    server_sock = g_server_sock;

    if (server_sock == -1) {
        printf("Failed to create socket\n");
#ifdef _WIN32
        cleanup_winsock();
#endif
        exit(1);
    }

    int opt = 1;
    setsockopt(server_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    server.sin_family = AF_INET;
    server.sin_addr.s_addr = htonl(INADDR_ANY);
    if (port > 0) {
        server.sin_port = htons(port);
    } else {
        server.sin_port = 0;
    }

    if (bind(server_sock, (struct sockaddr*) &server, sizeof server) == -1) {
        printf("Failed to bind socket\n");
#ifdef _WIN32
        cleanup_winsock();
#endif
        exit(1);
    }

    length = sizeof server;
    if (getsockname(server_sock, (struct sockaddr*) &server, &length) == -1) {
        printf("Failed to get socket name\n");
#ifdef _WIN32
        cleanup_winsock();
#endif
        exit(1);
    }

    printf("Battleship server started on port %d\n", ntohs(server.sin_port));

    if (listen(server_sock, 5) == -1) {
        printf("Failed to listen on socket\n");
#ifdef _WIN32
        cleanup_winsock();
#endif
        exit(1);
    }

    fds[0].fd = server_sock;
    fds[0].events = POLLIN;

    printf("Waiting for connections...\n");

    while (1) {
        int poll_count = poll(fds, nfds, -1);

        if (poll_count == -1) {
            if (errno == EINTR) continue;
            printf("Poll error\n");
#ifdef _WIN32
            cleanup_winsock();
#endif
            exit(1);
        }

        if (fds[0].revents & POLLIN) {
            int new_client = accept(server_sock, NULL, NULL);
            if (new_client == -1) {
                printf("Accept error\n");
            } else if (nfds < MAX_CLIENTS + 1) {
                int client_slot = -1;
                for (i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i].fd == -1) {
                        client_slot = i;
                        break;
                    }
                }

                if (client_slot != -1) {
                    clients[client_slot].fd = new_client;
                    clients[client_slot].session_id = -1;
                    clients[client_slot].player_number = 0;
                    clients[client_slot].ready = 0;
                    create_game_field(clients[client_slot].field);
                    initialize_ships(&clients[client_slot].ship_data);

                    fds[nfds].fd = new_client;
                    fds[nfds].events = POLLIN;
                    fds[nfds].revents = 0;

                    printf("New client connected: fd=%d\n", new_client);

                    send_packet_by_parts(new_client, "WELCOME", "Welcome to Battleship! Choose a session:", NULL);
                    send_session_list(&clients[client_slot]);
                    send_leaderboard_to_client(new_client);

                    nfds++;
                } else {
                    printf("Max clients reached. Rejecting connection.\n");
                    sock_close(new_client);
                }
            } else {
                printf("Max clients reached. Rejecting connection.\n");
                sock_close(new_client);
            }
        }

        for (i = 1; i < nfds; i++) {
            if (fds[i].fd == -1) continue;

            if (fds[i].revents & (POLLIN | POLLHUP)) {
                packet_t pkt;
                memset(&pkt, 0, sizeof(pkt));
                int got = recv_packet_fd(fds[i].fd, &pkt);
                if (got <= 0) {
                    printf("Client %d disconnected\n", fds[i].fd);

                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (clients[j].fd == fds[i].fd) {
                            disconnect_client(&clients[j]);
                            break;
                        }
                    }

                    for (int k = i; k < nfds - 1; k++) {
                        fds[k] = fds[k + 1];
                    }
                    nfds--;
                    i--;
                    continue;
                    
                } else {
                    for (int j = 0; j < MAX_CLIENTS; j++) {
                        if (clients[j].fd == fds[i].fd) {
                            process_client_packet(&clients[j], &pkt);
                            break;
                        }
                    }
                }
            }
        }

        static int compact_counter = 0;
        if (++compact_counter > 100) {
            compact_counter = 0;
            int write_index = 1;
            for (i = 1; i < nfds; i++) {
                if (fds[i].fd != -1) {
                    if (write_index != i) {
                        fds[write_index] = fds[i];
                        fds[i].fd = -1;
                    }
                    write_index++;
                }
            }
            nfds = write_index;
        }
    }

    for (i = 1; i < nfds; i++) {
        if (fds[i].fd != -1) sock_close(fds[i].fd);
    }
    sock_close(server_sock);
    remove_pid_file();
#ifdef _WIN32
    cleanup_winsock();
#endif
    return 0;
}
