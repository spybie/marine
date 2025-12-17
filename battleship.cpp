#include "battleship.h"
#include <iostream>
#include <fstream>
#include <algorithm>

// Reads leaderboard, updates score, writes file back
void update_leaderboard(const char* nickname) {
    std::ifstream inFile(LEADERBOARD_FILE);
    std::string names[512];
    int scores[512];
    int count = 0;

    if (inFile.is_open()) {
        while (inFile >> names[count] >> scores[count]) {
            count++;
        }
        inFile.close();
    }

    // Search for nickname
    int found = -1;
    for (int i = 0; i < count; i++) {
        if (names[i] == nickname) {
            found = i;
            break;
        }
    }

    if (found >= 0) {
        scores[found] += 1;
    } else {
        names[count] = nickname;
        scores[count] = 1;
        count++;
    }

    // Save leaderboard back to file
    std::ofstream outFile(LEADERBOARD_FILE);
    if (!outFile.is_open()) return;

    for (int i = 0; i < count; i++) {
        outFile << names[i] << " " << scores[i] << std::endl;
    }
    outFile.close();
}

// Creating an empty game field
void create_game_field(Field field) {
    for (int i = 0; i < FIELD_SIZE; i++) {
        for (int j = 0; j < FIELD_SIZE; j++) {
            field[i][j] = 0;
        }
    }
}

// Creating arrays for warships and filling them with zeroes.
void initialize_ships(struct ships* ship_data) {
    for (int i = 0; i < 4; i++) {
        ship_data->ship_41[0][i] = 0;
        ship_data->ship_41[1][i] = 0;
    }
    for (int i = 0; i < 3; i++) {
        ship_data->ship_31[0][i] = 0;
        ship_data->ship_31[1][i] = 0;
        ship_data->ship_32[0][i] = 0;
        ship_data->ship_32[1][i] = 0;
    }
    for (int i = 0; i < 2; i++) {
        ship_data->ship_21[0][i] = 0;
        ship_data->ship_21[1][i] = 0;
        ship_data->ship_22[0][i] = 0;
        ship_data->ship_22[1][i] = 0;
        ship_data->ship_23[0][i] = 0;
        ship_data->ship_23[1][i] = 0;
    }
    ship_data->ship_11[0][0] = 0;
    ship_data->ship_11[1][0] = 0;
    ship_data->ship_12[0][0] = 0;
    ship_data->ship_12[1][0] = 0;
    ship_data->ship_13[0][0] = 0;
    ship_data->ship_13[1][0] = 0;
    ship_data->ship_14[0][0] = 0;
    ship_data->ship_14[1][0] = 0;
}

void update_ship_data(struct ships* ship_data, int ship_idx, int segment, int x, int y) {
    unsigned char coord = (x - 1) * 10 + (y - 1);

    switch (ship_idx) {
        case 0: { ship_data->ship_41[0][segment] = coord; break; }
        case 1: { ship_data->ship_31[0][segment] = coord; break; }
        case 2: { ship_data->ship_32[0][segment] = coord; break; }
        case 3: { ship_data->ship_21[0][segment] = coord; break; }
        case 4: { ship_data->ship_22[0][segment] = coord; break; }
        case 5: { ship_data->ship_23[0][segment] = coord; break; }
        case 6: { ship_data->ship_11[0][segment] = coord; break; }
        case 7: { ship_data->ship_12[0][segment] = coord; break; }
        case 8: { ship_data->ship_13[0][segment] = coord; break; }
        case 9: { ship_data->ship_14[0][segment] = coord; break; }
    }
}

// Autoplacing warships
void place_ships(Field field, struct ships* ship_data) {
    srand(time(NULL));

    int ship_lengths[] = {4, 3, 3, 2, 2, 2, 1, 1, 1, 1};
    int ship_placed = 0;

    for (int ship_idx = 0; ship_idx < 10; ship_idx++) {
        int n = ship_lengths[ship_idx];
        int placed = 0;

        while (!placed) {
            int outer_x = rand() % (FIELD_SIZE - n - 1);
            int outer_y = rand() % (FIELD_SIZE - n - 1);

            int collision = 0;
            for (int i = outer_x; i < outer_x + n + 2 && !collision; i++) {
                for (int j = outer_y; j < outer_y + n + 2 && !collision; j++) {
                    if (field[i][j] != 0) {
                        collision = 1;
                    }
                }
            }

            if (!collision) {
                int side = rand() % 4 + 1;
                int inner_x = outer_x + 1;
                int inner_y = outer_y + 1;

                if (side == 1) {
                    for (int i = 0; i < n; i++) {
                        field[inner_x][inner_y + i] = 1;
                        update_ship_data(ship_data, ship_idx, i, inner_x, inner_y + i);
                    }
                } else if (side == 2) {
                    for (int i = 0; i < n; i++) {
                        field[inner_x + i][inner_y + n - 1] = 1;
                        update_ship_data(ship_data, ship_idx, i, inner_x + i, inner_y + n - 1);
                    }
                } else if (side == 3) {
                    for (int i = 0; i < n; i++) {
                        field[inner_x + n - 1][inner_y + i] = 1;
                        update_ship_data(ship_data, ship_idx, i, inner_x + n - 1, inner_y + i);
                    }
                } else if (side == 4) {
                    for (int i = 0; i < n; i++) {
                        field[inner_x + i][inner_y] = 1;
                        update_ship_data(ship_data, ship_idx, i, inner_x + i, inner_y);
                    }
                }
                placed = 1;
                ship_placed++;
            }
        }
    }
}

// Manually placing warships
int convert_coordinates(const std::string& coord_string, int* x, int* y) {
    if (coord_string.length() < 2) return 0;
    if (coord_string.length() > 3) return 0;

    char col = toupper(coord_string[0]);
    char row = coord_string[1];

    if (col < 'A' || col > 'K') return 0;
    if (row < '1' || row > '9') return 0;
    if (col == 'J') return 0;

    if (col > 'I') col--;

    *x = col - 'A';

    if (coord_string.length() == 3 && coord_string[2] == '0' && coord_string[1] == '1') {
        *y = 9;
    } else if (coord_string.length() == 2) {
        *y = row - '1';
    } else {
        return 0;
    }

    return 1;
}

int place_ship_manual(Field field, struct ships* ship_data, const std::string& placement, int* ships_placed) {
    int x1, y1, x2, y2;

    size_t dash_pos = placement.find('-');
    if (dash_pos == std::string::npos) return 9;
    
    std::string token1 = placement.substr(0, dash_pos);
    std::string token2 = placement.substr(dash_pos + 1);

    if (token1.empty() || token2.empty()) return 9;

    if (!convert_coordinates(token1, &x1, &y1)) return 9;
    if (!convert_coordinates(token2, &x2, &y2)) return 9;

    if (x1 != x2 && y1 != y2) return 9;

    int length;
    if (x1 == x2) {
        length = abs(y2 - y1) + 1;
    } else {
        length = abs(x2 - x1) + 1;
    }

    if (length < 1 || length > 4) return 9;

    int max_ships[] = {1, 2, 3, 4};
    int length_index = 4 - length;

    if (ships_placed[length_index] >= max_ships[length_index]) {
        return 8;
    }

    int start_x, start_y, end_x, end_y;
    if (x1 == x2) {
        start_x = x1;
        end_x = x2;
        start_y = (y1 < y2) ? y1 : y2;
        end_y = (y1 < y2) ? y2 : y1;
    } else {
        start_y = y1;
        end_y = y2;
        start_x = (x1 < x2) ? x1 : x2;
        end_x = (x1 < x2) ? x2 : x1;
    }

    for (int x = start_x; x <= end_x; x++) {
        for (int y = start_y; y <= end_y; y++) {
            if (x < 0 || x >= PLAYABLE_SIZE || y < 0 || y >= PLAYABLE_SIZE) return 9;
            if (field[x + 1][y + 1] != 0) return 9;
        }
    }

    for (int x = start_x - 1; x <= end_x + 1; x++) {
        for (int y = start_y - 1; y <= end_y + 1; y++) {
            if (x >= 0 && x < PLAYABLE_SIZE && y >= 0 && y < PLAYABLE_SIZE) {
                if (field[x + 1][y + 1] != 0) return 9;
            }
        }
    }

    int ship_index;
    switch (length) {
        case 4: ship_index = 0; break;
        case 3: ship_index = 1 + ships_placed[1]; break;
        case 2: ship_index = 3 + ships_placed[2]; break;
        case 1: ship_index = 6 + ships_placed[3]; break;
    }

    int segment = 0;
    for (int x = start_x; x <= end_x; x++) {
        for (int y = start_y; y <= end_y; y++) {
            field[x + 1][y + 1] = 1;
            update_ship_data(ship_data, ship_index, segment, x + 1, y + 1);
            segment++;
        }
    }

    ships_placed[length_index]++;

    int total_ships = ships_placed[0] + ships_placed[1] + ships_placed[2] + ships_placed[3];
    if (total_ships == 10) {
        return 1;
    }

    return 0;
}

int check_ship_sunk(unsigned char hit_array[], int length) {
    for (int i = 0; i < length; i++) {
        if (hit_array[i] == 0) {
            return 0;
        }
    }
    return 1;
}

void mark_ship_area(char field[12][12], unsigned char ship_coords[], int length) {
    for (int i = 0; i < length; i++) {
        int x = (ship_coords[i] / 10) + 1;
        int y = (ship_coords[i] % 10) + 1;

        for (int dx = -1; dx <= 1; dx++) {
            for (int dy = -1; dy <= 1; dy++) {
                int nx = x + dx;
                int ny = y + dy;
                if (nx >= 1 && nx <= 10 && ny >= 1 && ny <= 10 && field[nx][ny] == 0) {
                    field[nx][ny] = 2;
                }
            }
        }
    }
}

void update_ship_hit(struct ships* ship_data, int x, int y) {
    unsigned char target_coord = (x - 1) * 10 + (y - 1);

    for (int i = 0; i < 4; i++) {
        if (ship_data->ship_41[0][i] == target_coord) {
            ship_data->ship_41[1][i] = 1;
            return;
        }
    }
    for (int i = 0; i < 3; i++) {
        if (ship_data->ship_31[0][i] == target_coord) {
            ship_data->ship_31[1][i] = 1;
            return;
        }
        if (ship_data->ship_32[0][i] == target_coord) {
            ship_data->ship_32[1][i] = 1;
            return;
        }
    }
    for (int i = 0; i < 2; i++) {
        if (ship_data->ship_21[0][i] == target_coord) {
            ship_data->ship_21[1][i] = 1;
            return;
        }
        if (ship_data->ship_22[0][i] == target_coord) {
            ship_data->ship_22[1][i] = 1;
            return;
        }
        if (ship_data->ship_23[0][i] == target_coord) {
            ship_data->ship_23[1][i] = 1;
            return;
        }
    }
    if (ship_data->ship_11[0][0] == target_coord) ship_data->ship_11[1][0] = 1;
    if (ship_data->ship_12[0][0] == target_coord) ship_data->ship_12[1][0] = 1;
    if (ship_data->ship_13[0][0] == target_coord) ship_data->ship_13[1][0] = 1;
    if (ship_data->ship_14[0][0] == target_coord) ship_data->ship_14[1][0] = 1;
}

void ship_explosion(char field[12][12], struct ships* ship_data) {
    if (check_ship_sunk(ship_data->ship_41[1], 4)) {
        mark_ship_area(field, ship_data->ship_41[0], 4);
    }
    if (check_ship_sunk(ship_data->ship_31[1], 3)) {
        mark_ship_area(field, ship_data->ship_31[0], 3);
    }
    if (check_ship_sunk(ship_data->ship_32[1], 3)) {
        mark_ship_area(field, ship_data->ship_32[0], 3);
    }
    if (check_ship_sunk(ship_data->ship_21[1], 2)) {
        mark_ship_area(field, ship_data->ship_21[0], 2);
    }
    if (check_ship_sunk(ship_data->ship_22[1], 2)) {
        mark_ship_area(field, ship_data->ship_22[0], 2);
    }
    if (check_ship_sunk(ship_data->ship_23[1], 2)) {
        mark_ship_area(field, ship_data->ship_23[0], 2);
    }
    if (check_ship_sunk(ship_data->ship_11[1], 1)) {
        mark_ship_area(field, ship_data->ship_11[0], 1);
    }
    if (check_ship_sunk(ship_data->ship_12[1], 1)) {
        mark_ship_area(field, ship_data->ship_12[0], 1);
    }
    if (check_ship_sunk(ship_data->ship_13[1], 1)) {
        mark_ship_area(field, ship_data->ship_13[0], 1);
    }
    if (check_ship_sunk(ship_data->ship_14[1], 1)) {
        mark_ship_area(field, ship_data->ship_14[0], 1);
    }
}

int simple_shot(char field[12][12], struct ships* ship_data, const std::string& coord_string) {
    int x, y;
    if (!convert_coordinates(coord_string, &x, &y)) {
        return -1;
    }

    x++;
    y++;

    if (field[x][y] == 1) {
        field[x][y] = 3;
        update_ship_hit(ship_data, x, y);
        ship_explosion(field, ship_data);
        return 1;
    } else if (field[x][y] == 0) {
        field[x][y] = 2;
        return 0;
    }

    return -1;
}

// Display functions
void print_full_field(Field field) {
    char letters[] = "-ABCDEFGHIK-";
    printf("-  -  ");
    for (int i = 1; i < 10; i++) printf("%d  ", i);
    printf("10 -\n");
    for (int i = 0; i < FIELD_SIZE; i++) {
        printf("%c  ", letters[i]);
        for (int j = 1; j < 11; j++) {
            if (field[i][j] == 1) {
                printf("□  ");
            } else if (field[i][j] == 2) {
                printf("◉  ");
            } else if (field[i][j] == 3) {
                printf("🗵  ");
            } else {
                printf(".  ");
            }
        }
        printf("\n");
    }
}

void print_field(Field field) {
    char letters[] = "-ABCDEFGHIK";
    printf("-  ");
    for (int i = 1; i < 10; i++) printf("%d  ", i);
    printf("10\n");
    for (int i = 1; i < 11; i++) {
        printf("%c  ", letters[i]);
        for (int j = 1; j < 11; j++) {
            if (field[i][j] == 1) {
                printf("□  ");
            } else if (field[i][j] == 2) {
                printf("◉  ");
            } else if (field[i][j] == 3) {
                printf("🗵  ");
            } else {
                printf(".  ");
            }
        }
        printf("\n");
    }
}

void print_two_fields_side_by_side(Field left, Field right) {
    char letters[] = "-ABCDEFGHIK";

    printf("\n=== YOUR FIELD ========================|========= ENEMY FIELD ================\n\n");

    printf("- ");
    for (int i = 1; i < PLAYABLE_SIZE; i++) printf("%2d ", i);
    printf(" 10 ");
    printf("      |     ");
    printf(" - ");
    for (int i = 1; i < PLAYABLE_SIZE; i++) printf("%2d ", i);
    printf(" 10\n");

    for (int row = 1; row <= PLAYABLE_SIZE; row++) {
        printf("%c  ", letters[row]);
        for (int col = 1; col <= PLAYABLE_SIZE; col++) {
            if (left[row][col] == 1) printf("□  ");
            else if (left[row][col] == 2) printf("◉  ");
            else if (left[row][col] == 3) printf("🗵  ");
            else printf (".  ");
        }
        printf("      |      ");
        printf("%c  ", letters[row]);
        for (int col = 1; col <= PLAYABLE_SIZE; col++) {
            if (right[row][col] == 2) printf("◉  ");
            else if (right[row][col] == 3) printf("🗵  ");
            else printf (".  ");
        }
        printf("\n");
    }
    printf("\n");
}

void print_ships(struct ships* ship_data) {
    char letters[] = "ABCDEFGHIK";

    printf("4-cell ship:\n");
    for (int i = 0; i < 4; i++) {
        int coord = 11 + ship_data->ship_41[0][i];
        int x = coord / 10;
        int y = coord % 10;
        printf("  %c%d\n", letters[x - 1], y);
    }

    printf("3-cell ships:\n");
    for (int i = 0; i < 3; i++) {
        int coord = 11 + ship_data->ship_31[0][i];
        int x = coord / 10;
        int y = coord % 10;
        printf("  %c%d\n", letters[x - 1], y);
    }
    printf("-----\n");
    for (int i = 0; i < 3; i++) {
        int coord = 11 + ship_data->ship_32[0][i];
        int x = coord / 10;
        int y = coord % 10;
        printf("  %c%d\n", letters[x - 1], y);
    }

    printf("2-cell ships:\n");
    for (int i = 0; i < 2; i++) {
        int coord = 11 + ship_data->ship_21[0][i];
        int x = coord / 10;
        int y = coord % 10;
        printf("  %c%d\n", letters[x - 1], y);
    }
    printf("-----\n");
    for (int i = 0; i < 2; i++) {
        int coord = 11 + ship_data->ship_22[0][i];
        int x = coord / 10;
        int y = coord % 10;
        printf("  %c%d\n", letters[x - 1], y);
    }
    printf("-----\n");
    for (int i = 0; i < 2; i++) {
        int coord = 11 + ship_data->ship_23[0][i];
        int x = coord / 10;
        int y = coord % 10;
        printf("  %c%d\n", letters[x - 1], y);
    }

    printf("1-cell ships:\n");
    int coord = 11 + ship_data->ship_11[0][0];
    int x = coord / 10;
    int y = coord % 10;
    printf("  %c%d\n", letters[x - 1], y);

    printf("-----\n");
    coord = 11 + ship_data->ship_12[0][0];
    x = coord / 10;
    y = coord % 10;
    printf("  %c%d\n", letters[x - 1], y);

    printf("-----\n");
    coord = 11 + ship_data->ship_13[0][0];
    x = coord / 10;
    y = coord % 10;
    printf("  %c%d\n", letters[x - 1], y);

    printf("-----\n");
    coord = 11 + ship_data->ship_14[0][0];
    x = coord / 10;
    y = coord % 10;
    printf("  %c%d\n", letters[x - 1], y);
}

void print_game_session(struct game_session* session, int player_number) {
    if (!session) return;

    struct client_info* my_client =
        (player_number == 1) ? session->player1 : session->player2;
    struct client_info* enemy_client =
        (player_number == 1) ? session->player2 : session->player1;

    if (!my_client || !enemy_client) return;

    // Build fog-of-war field for enemy
    Field enemy_fog;
    create_game_field(enemy_fog);

    for (int i = 1; i <= PLAYABLE_SIZE; i++) {
        for (int j = 1; j <= PLAYABLE_SIZE; j++) {
            if (enemy_client->field[i][j] == 2 || enemy_client->field[i][j] == 3) {
                enemy_fog[i][j] = enemy_client->field[i][j];  // visible hit/miss
            } else {
                enemy_fog[i][j] = 0;  // hidden
            }
        }
    }

    print_two_fields_side_by_side(my_client->field, enemy_fog);
}

int all_ships_sunk(struct ships* ship_data) {
    // If any ship segment is not marked as hit (3), ship still afloat
    // 4-deck ship
    for (int i = 0; i < 4; i++)
        if (ship_data->ship_41[1][i] == 0) return 0;

    // 3-deck ships
    for (int i = 0; i < 3; i++)
        if (ship_data->ship_31[1][i] == 0) return 0;
    for (int i = 0; i < 3; i++)
        if (ship_data->ship_32[1][i] == 0) return 0;

    // 2-deck ships
    for (int i = 0; i < 2; i++)
        if (ship_data->ship_21[1][i] == 0) return 0;
    for (int i = 0; i < 2; i++)
        if (ship_data->ship_22[1][i] == 0) return 0;
    for (int i = 0; i < 2; i++)
        if (ship_data->ship_23[1][i] == 0) return 0;

    // 1-deck ships
    if (ship_data->ship_11[1][0] == 0) return 0;
    if (ship_data->ship_12[1][0] == 0) return 0;
    if (ship_data->ship_13[1][0] == 0) return 0;
    if (ship_data->ship_14[1][0] == 0) return 0;

    return 1; // All ships sunk
}
