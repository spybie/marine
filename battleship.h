#ifndef BATTLESHIP_H
#define BATTLESHIP_H

#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <cctype>
#include <string>

/* --- PACKET CONFIGURATION --- */
#define PACKET_COMMAND_SIZE 32
#define PACKET_ARG_SIZE_1 384
#define PACKET_ARG_SIZE_2 96
#define PACKET_SIZE (PACKET_COMMAND_SIZE + PACKET_ARG_SIZE_1 + PACKET_ARG_SIZE_2)

#define FIELD_SIZE 12
#define PLAYABLE_SIZE 10
#define LEADERBOARD_FILE "leaderboard.txt"
#define MAX_SESSIONS 10
#define MAX_CLIENTS 20

// Packet type (fixed size PACKET_SIZE bytes)
typedef struct {
    char command[PACKET_COMMAND_SIZE];
    char arg1[PACKET_ARG_SIZE_1];
    char arg2[PACKET_ARG_SIZE_2];
} packet_t;

// Game field type
typedef char Field[FIELD_SIZE][FIELD_SIZE];

// Ship data structure
struct ships {
    unsigned char ship_41[2][4];
    unsigned char ship_31[2][3];
    unsigned char ship_32[2][3];
    unsigned char ship_21[2][2];
    unsigned char ship_22[2][2];
    unsigned char ship_23[2][2];
    unsigned char ship_11[2][1];
    unsigned char ship_12[2][1];
    unsigned char ship_13[2][1];
    unsigned char ship_14[2][1];
};

// Client and session structures
struct client_info {
    int fd;
    int session_id;
    int player_number;
    int ready;
    char nickname[64];
    Field field;
    struct ships ship_data;
};

struct game_session {
    int id;
    client_info* player1;
    client_info* player2;
    int game_started;
    int current_turn; // 1 or 2
};

// Core game functions
void create_game_field(Field field);
void initialize_ships(struct ships* ship_data);
void update_ship_data(struct ships* ship_data, int ship_idx, int segment, int x, int y);
void place_ships(Field field, struct ships* ship_data);

// Manual placement functions
int convert_coordinates(const std::string& coord_string, int* x, int* y);
int place_ship_manual(Field field, struct ships* ship_data, const std::string& placement, int* ships_placed);

// Shooting mechanics
int simple_shot(Field field, struct ships* ship_data, const std::string& coord_string);
void update_ship_hit(struct ships* ship_data, int x, int y);
void ship_explosion(Field field, struct ships* ship_data);
int check_ship_sunk(unsigned char hit_array[], int length);
void mark_ship_area(Field field, unsigned char ship_coords[], int length);
int all_ships_sunk(struct ships* ship_data);

// Display functions
void update_leaderboard(const char* nickname);
void print_full_field(Field field);
void print_field(Field field);
void print_two_fields_side_by_side(Field left, Field right);
void print_ships(struct ships* ship_data);
void print_game_session(struct game_session* session, int player_number);

// Server helper used by server/client implementation
void send_message(int fd, const char* message);

#endif // BATTLESHIP_H
