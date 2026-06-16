#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>
#include <testkit.h>
#include "labyrinth.h"

void printUsage(void);

int main(int argc, char *argv[]) {
    // --version must be the sole argument
    if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
        if (argc != 2) return 1;
        printf("%s\n", VERSION_INFO);
        return 0;
    }

    char *mapFile = NULL, *playerStr = NULL, *moveDir = NULL;
    for (int i = 1; i < argc; i++) {
        if ((strcmp(argv[i], "--map") == 0 || strcmp(argv[i], "-m") == 0) && i + 1 < argc)
            mapFile = argv[++i];
        else if ((strcmp(argv[i], "--player") == 0 || strcmp(argv[i], "-p") == 0) && i + 1 < argc)
            playerStr = argv[++i];
        else if (strcmp(argv[i], "--move") == 0 && i + 1 < argc)
            moveDir = argv[++i];
        else { printUsage(); return 1; }
    }

    if (!mapFile || !playerStr) { printUsage(); return 1; }
    if (strlen(playerStr) != 1 || !isValidPlayer(playerStr[0])) return 1;
    char playerId = playerStr[0];

    Labyrinth labyrinth;
    if (!loadMap(&labyrinth, mapFile)) return 1;
    if (!isConnected(&labyrinth)) return 1;

    if (moveDir) {
        // Spawn player at first empty space if not on map
        if (findPlayer(&labyrinth, playerId).row == -1) {
            Position spawn = findFirstEmptySpace(&labyrinth);
            if (spawn.row == -1) return 1;
            labyrinth.map[spawn.row][spawn.col] = playerId;
        }
        // Only save on successful move
        if (!movePlayer(&labyrinth, playerId, moveDir)) return 1;
        if (!saveMap(&labyrinth, mapFile)) return 1;
    } else {
        for (int r = 0; r < labyrinth.rows; r++)
            printf("%s\n", labyrinth.map[r]);
    }
    return 0;
}

void printUsage() {
    printf("Usage:\n");
    printf("  labyrinth --map map.txt --player id\n");
    printf("  labyrinth -m map.txt -p id\n");
    printf("  labyrinth --map map.txt --player id --move direction\n");
    printf("  labyrinth --version\n");
}

bool isValidPlayer(char playerId) {
    return playerId >= '0' && playerId <= '9';
}

bool loadMap(Labyrinth *labyrinth, const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) return false;

    int rows = 0, cols = -1;
    char line[MAX_COLS + 2]; // +1 for '\n', +1 for '\0'

    while (fgets(line, sizeof(line), f) && rows < MAX_ROWS) {
        int len = (int)strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[--len] = '\0';

        if (cols == -1) {
            cols = len;
        } else if (len != cols) {
            fclose(f);
            return false; // uneven row lengths rejected
        }
        memcpy(labyrinth->map[rows], line, (size_t)len + 1);
        rows++;
    }
    fclose(f);

    if (rows == 0) return false;
    labyrinth->rows = rows;
    labyrinth->cols = cols;
    return true;
}

Position findPlayer(Labyrinth *labyrinth, char playerId) {
    for (int r = 0; r < labyrinth->rows; r++)
        for (int c = 0; c < labyrinth->cols; c++)
            if (labyrinth->map[r][c] == playerId)
                return (Position){r, c};
    return (Position){-1, -1};
}

Position findFirstEmptySpace(Labyrinth *labyrinth) {
    for (int r = 0; r < labyrinth->rows; r++)
        for (int c = 0; c < labyrinth->cols; c++)
            if (labyrinth->map[r][c] == '.') return (Position){r, c};
    return (Position){-1, -1};
}

bool isEmptySpace(Labyrinth *labyrinth, int row, int col) {
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols)
        return false;
    return labyrinth->map[row][col] == '.';
}

bool movePlayer(Labyrinth *labyrinth, char playerId, const char *direction) {
    Position pos = findPlayer(labyrinth, playerId);
    if (pos.row == -1) return false;

    int dr = 0, dc = 0;
    if      (strcmp(direction, "up")    == 0) dr = -1;
    else if (strcmp(direction, "down")  == 0) dr =  1;
    else if (strcmp(direction, "left")  == 0) dc = -1;
    else if (strcmp(direction, "right") == 0) dc =  1;
    else return false; // unknown direction — map left unchanged

    int nr = pos.row + dr, nc = pos.col + dc;
    if (!isEmptySpace(labyrinth, nr, nc)) return false; // wall, OOB, or occupied

    labyrinth->map[pos.row][pos.col] = '.';
    labyrinth->map[nr][nc] = playerId;
    return true;
}

bool saveMap(Labyrinth *labyrinth, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) return false;
    for (int r = 0; r < labyrinth->rows; r++) {
        fputs(labyrinth->map[r], f);
        fputc('\n', f);
    }
    fclose(f);
    return true;
}

// Check if all empty spaces are connected using DFS
void dfs(Labyrinth *labyrinth, int row, int col, bool visited[MAX_ROWS][MAX_COLS]) {
    if (row < 0 || row >= labyrinth->rows || col < 0 || col >= labyrinth->cols) return;
    if (visited[row][col] || labyrinth->map[row][col] == '#') return;
    visited[row][col] = true;
    dfs(labyrinth, row - 1, col, visited);
    dfs(labyrinth, row + 1, col, visited);
    dfs(labyrinth, row, col - 1, visited);
    dfs(labyrinth, row, col + 1, visited);
}

bool isConnected(Labyrinth *labyrinth) {
    bool visited[MAX_ROWS][MAX_COLS] = {false};

    // Seed DFS from the first non-wall cell
    int sr = -1, sc = -1;
    for (int r = 0; r < labyrinth->rows && sr == -1; r++)
        for (int c = 0; c < labyrinth->cols && sr == -1; c++)
            if (labyrinth->map[r][c] != '#') { sr = r; sc = c; }

    if (sr == -1) return true; // all walls — trivially connected
    dfs(labyrinth, sr, sc, visited);

    // Every non-wall cell must have been reached
    for (int r = 0; r < labyrinth->rows; r++)
        for (int c = 0; c < labyrinth->cols; c++)
            if (labyrinth->map[r][c] != '#' && !visited[r][c]) return false;
    return true;
}
