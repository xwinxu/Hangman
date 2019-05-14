#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <time.h>
#include <signal.h>

#include "socket.h"
#include "gameplay.h"


#ifndef PORT
    #define PORT 57715
#endif
#define MAX_QUEUE 5

/* The set of socket descriptors for select to monitor.
 * This is a global variable because we need to remove socket descriptors
 * from allset when a write to a socket fails.
 */
fd_set allset;

/* Indicate that a client has disconnected given it's not select()-ed out*/
void disconnect_client(struct client *client) {
    client->is_connected = 0;
}

///////////////////// I/O HELPERS /////////////////////

/* Finds the network newline from user input */
int find_network_newline(char *buf, char *in_ptr) {
    int index = 0;
    int found = 0;
    while (buf != in_ptr) {
        if (*buf == '\r') found = 1;
        else if (found && *buf == '\n') return index + 1;
        else found = 0;
        buf++;
        index++;
    }
    return -1;
}

/* Wrapper for writing to client with error checking */
int write_to_client(struct client *client, char *message) {
    int nbytes;
    if ((nbytes = write(client->fd, message, strlen(message) + 1)) <= 0) {
        fprintf(stderr, "Write to client @ %s failed.\n", 
                inet_ntoa(client->ipaddr));
        disconnect_client(client);
        return -1;
    }

    return nbytes;
}

/* Read input from a client in all occassions with error checking */
int read_from_client(struct client *client, char *message) {
    int nbytes;
    if((nbytes = read(client->fd, client->in_ptr, 
        MAX_MSG - (client->in_ptr - client->inbuf))) <= 0) {
        fprintf(stderr, "Read from client @ %s failed.\n", 
                inet_ntoa(client->ipaddr));
        // if user inputs beyond the MAX_MSG length, to prevent seg fault
        write_to_client(client, "Something went wrong :( Try again\r\n");
        disconnect_client(client);
        return -1;
    }
    printf("[%d] Read %d bytes\n", client->fd, nbytes);
    client->in_ptr += nbytes;

    int netline;
    int is_empty = 1;
    while((netline = find_network_newline(client->inbuf, client->in_ptr)) != -1) {
        client->inbuf[netline - 2] = '\0';
        printf("Found newline %s\n", client->inbuf);
        strcpy(message, client->inbuf);
        *(client->in_ptr) = '\0';
        strcpy(client->inbuf, client->inbuf + netline);
        client->in_ptr -= netline;
        is_empty = 0;
    }
    // never populated message, don't use it (is_empty is 1)
    return is_empty ? -1 : nbytes;
}

/* Send the message in outbuf to specific clients */
void broadcast(struct game_state *game, char *outbuf,
               struct client *nosend, char *specmsg) {
    for(struct client *curr = game->head; curr != NULL 
         && outbuf != NULL; curr = curr->next) {
        if(curr == nosend) continue;
        write_to_client(curr, outbuf);
    }

    if(nosend != NULL && specmsg != NULL) write_to_client(nosend, specmsg);
}

/* Print out the board state to members */
void announce_board(struct game_state *game, struct client *client) {
    char message[MAX_MSG];
    status_message(message, game);
    if (client != NULL) write_to_client(client, message);
    else broadcast(game, status_message(message, game), NULL, NULL);
}

/* Announce the current turn in the correct tense */
void announce_turn(struct game_state *game, struct client *client) {
    char other_msg[MAX_MSG];
    sprintf(other_msg, "It's %s's turn!\r\n", game->has_next_turn->name);
    char current_msg[MAX_MSG];
    sprintf(current_msg, "It's your turn!\r\nEnter your guess: ");

    if(client != NULL) {
        if(client == game->has_next_turn) write_to_client(client, current_msg);
        else {
            write_to_client(client, other_msg);
            printf("%s\n", other_msg);
        }
    } else {
        broadcast(game, other_msg, game->has_next_turn, current_msg);
        printf("%s\n", other_msg);
    }
}

/* Announce the winner to all active clients */
void announce_winner(struct game_state *game, struct client *winner) {
    char message[MAX_MSG];
    sprintf(message, "Game over! %s won!\r\n  \r\n  \r\n", winner->name);
    printf("%s\n", message);
    broadcast(game, message, winner, WIN_MSG);
}

/* Change the bit vector and broadcast message to all */
void announce_guess(struct game_state *game, char letter) {
    char msgAll[MAX_MSG];
    char msgCurr[MAX_MSG];
    char letter_str[] = {letter, '\0'};
    sprintf(msgCurr, "You guessed %s!\r\n", letter_str);
    sprintf(msgAll, "%s guessed %s!\r\n", game->has_next_turn->name, letter_str);
    broadcast(game, msgAll, game->has_next_turn, msgCurr);
}

/* Indicate that it's an incorrect guess */
void announce_incorrect_guess(struct game_state *game, char letter) {
    char msgAll[MAX_MSG];
    char letter_str[] = {letter, '\0'};
    sprintf(msgAll, "The letter %s is not in the word!\r\n", letter_str);
    printf("%s", msgAll);
    broadcast(game, msgAll, game->has_next_turn, msgAll);
}

/* Broadcast when game is over */
void announce_no_guesses(struct game_state *game) {
    char msgAll[MAX_MSG] = "Game over, no more guesses!\r\n";
    char msgWord[MAX_MSG];
    sprintf(msgWord, "The word was %s. \r\n  \r\n  \r\n", game->word);
    printf("%s\n", msgAll);
    broadcast(game, msgAll, game->has_next_turn, msgAll);
    broadcast(game, msgWord, game->has_next_turn, msgWord);
}

////////////////// END I/O HELPERS ////////////////////

/* Find the client object in the two linked lists */
int find_client(struct client *players, struct client *new_players, 
                int cur_fd, struct client **client, struct client **prev) {
    struct client *cur;
    struct client *cur_prev = NULL;
    for(cur = players; cur != NULL && cur->fd != cur_fd; 
        cur_prev = cur, cur = cur->next);
    if(cur != NULL) {
        *client = cur;
        *prev = cur_prev;
        return 1;
    }

    cur_prev = NULL;
    for(cur = new_players; cur != NULL && cur->fd != cur_fd; 
        cur_prev = cur, cur = cur->next);
    if(cur != NULL) {
        *client = cur;
        *prev = cur_prev;
        return 2;
    }

    return 0;
}


/* Move the has_next_turn pointer to the next active client */
void advance_turn(struct game_state *game) {
    game->has_next_turn = game->has_next_turn->next;
    if (game->has_next_turn == NULL) game->has_next_turn = game->head;
}

/* Returns if guess was right, and updates the current game state */
void guess(struct game_state *game, char letter) {
    int num_unlocked = 0;
    for(int i = 0; i < strlen(game->word); i++) {
        if(game->word[i] == letter) {
            game->guess[i] = letter;
            num_unlocked++;
        }
    }

    game->letters_guessed[letter - 'a'] = 1;
    announce_guess(game, letter);

    if(!num_unlocked) {
        game->guesses_left -= 1;
        announce_incorrect_guess(game, letter);
        advance_turn(game);
    }
}

/* Indicates if game is over after guess is made 
0 if progress, 1 if done, -1 if no guesses left 
*/
int get_game_state(struct game_state *game) {
    int count = 0;
    int wordLen = strlen(game->guess);
    if(game->guesses_left == 0) return -1;
    for(int i = 0; i < wordLen; i++) {
        if(game->guess[i] != '-') count++;
    }
    return (wordLen == count);
}

/* Returns the letter if valid guess, otherwise empty */
char validate_input(struct game_state *game, char *msg) {
    if(strlen(msg) == 1 && 'a' <= msg[0] && 'z' >= msg[0] 
       && game->letters_guessed[msg[0] - 'a'] == 0) {
        return *msg;
    } else {
        return '\0';
    }
}

/* Indicates if the player cur_fd is the one to play */
int is_curr_fd(struct game_state *game, int cur_fd) {
    if(game->has_next_turn->fd == cur_fd) {
        return 1;
    }
    return 0;
}

/* Indicates if the username entered has already been taken
0 if didn't find name, 1 if found same name */
int is_valid_username(struct game_state *game, char *name) {
    struct client *cur;
    for(cur = game->head; cur != NULL; cur = cur->next) {
        if(strcmp(cur->name, name) == 0) return 0;
    }
    return strlen(name) >= 1 && name[0] != '\0';
}



/* Add a client to the head of the linked list */
void add_player(struct client **top, int fd, struct in_addr addr) {
    struct client *p = malloc(sizeof(struct client));

    if (!p) {
        perror("malloc");
        exit(1);
    }

    printf("Adding client %s\n", inet_ntoa(addr));

    p->fd = fd;
    p->ipaddr = addr;
    p->name[0] = '\0';
    p->in_ptr = p->inbuf;
    p->inbuf[0] = '\0';
    p->next = *top;
    p->is_connected = 1;
    *top = p;
}

/* Logic for an active client player */
void player_action(struct game_state *game, struct client *client, char *message) {
    if(!is_curr_fd(game, client->fd)) {
        printf("%s tried to guess out of turn\n", client->name);
        write_to_client(client, "It's not your turn to guess.\r\n");
        return;
    }

    char letter;
    if((letter = validate_input(game, message)) == '\0') {
        printf("%s made an invalid guess, trying again...\n", client->name);
        write_to_client(client, "Your guess is invalid!\r\nYour guess again: ");
        return;
    }

    guess(game, letter);

    // evaluate game state
    int gameCode;
    if((gameCode = get_game_state(game)) == 1) {
        announce_winner(game, game->has_next_turn);
        broadcast(game, NEW_MSG, NULL, NULL);
        init_game(game, NULL);
    } else if(gameCode == -1) {
        announce_no_guesses(game);
        broadcast(game, NEW_MSG, NULL, NULL);
        init_game(game, NULL);
    }
    announce_board(game, NULL);
    announce_turn(game, NULL);
}

/* Logic for a new player client */
void new_player_action(struct game_state *game, struct client **new_players,
                struct client *client, struct client *prev, char *message) {
    if(!is_valid_username(game, message)) {
        write_to_client(client, "Username already taken. Try again: ");
        return;
    }

    strcpy(client->name, message);

    // remove from new players list and insert into active players
    if(prev != NULL) prev->next = client->next;
    else *new_players = client->next;
    client->next = game->head;
    game->head = client;

    char join_message[MAX_MSG];
    sprintf(join_message, "\r\n%s has just joined!\r\n", client->name);
    printf("%s has just joined\n", client->name);
    broadcast(game, join_message, client, NULL);

    announce_board(game, client);
    if (client->next == NULL) game->has_next_turn = client;
    announce_turn(game, NULL);
}

/* Removes client from the linked list and closes its socket.
 * Also removes socket descriptor from allset 
 */
void remove_player(struct client **top, int fd) {
    struct client **p;

    for (p = top; *p && (*p)->fd != fd; p = &(*p)->next);
    // Now, p points to (1) top, or (2) a pointer to another client
    // This avoids a special case for removing the head of the list
    if (*p) {
        struct client *t = (*p)->next;
        printf("Removing client %d %s\n", fd, inet_ntoa((*p)->ipaddr));
        FD_CLR((*p)->fd, &allset);
        close((*p)->fd);
        free(*p);
        *p = t;
    } else {
        fprintf(stderr, "Trying to remove fd %d, but don't know about it\n", fd);
    }
}

int main(int argc, char **argv) {
    int clientfd, maxfd, nready;
    struct sockaddr_in q;
    fd_set rset;
    
    if(argc != 2){
        fprintf(stderr,"Usage: %s <dictionary filename>\n", argv[0]);
        exit(1);
    }

    struct sigaction sa;
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = 0;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        perror("sigaction");
        exit(1);
    }

    // Create and initialize the game state
    struct game_state game;

    srandom((unsigned int)time(NULL));
    // Set up the file pointer outside of init_game because we want to 
    // just rewind the file when we need to pick a new word
    game.dict.fp = NULL;
    game.dict.size = get_file_length(argv[1]);

    init_game(&game, argv[1]);
    
    // head and has_next_turn also don't change when a subsequent game is
    // started so we initialize them here.
    game.head = NULL;
    game.has_next_turn = NULL;
    
    /* A list of client who have not yet entered their name.  This list is
     * kept separate from the list of active players in the game, because
     * until the new playrs have entered a name, they should not have a turn
     * or receive broadcast messages.  In other words, they can't play until
     * they have a name.
     */
    struct client *new_players = NULL;
    
    struct sockaddr_in *server = init_server_addr(PORT);
    int listenfd = set_up_server_socket(server, MAX_QUEUE);
    
    // initialize allset and add listenfd to the
    // set of file descriptors passed into select
    FD_ZERO(&allset);
    FD_SET(listenfd, &allset);
    // maxfd identifies how far into the set to search
    maxfd = listenfd;

    while (1) {
        // make a copy of the set before we pass it into select
        rset = allset;
        nready = select(maxfd + 1, &rset, NULL, NULL, NULL);
        if (nready == -1) {
            perror("select");
            continue;
        }

        if (FD_ISSET(listenfd, &rset)){
            printf("A new client is connecting\n");
            clientfd = accept_connection(listenfd);

            FD_SET(clientfd, &allset);
            if (clientfd > maxfd) {
                maxfd = clientfd;
            }
            printf("Connection from %s\n", inet_ntoa(q.sin_addr));
            add_player(&new_players, clientfd, q.sin_addr);
            char *greeting = WELCOME_MSG;
            if(write(clientfd, greeting, strlen(greeting)) == -1) {
                fprintf(stderr, "Write to client %s failed\n", inet_ntoa(q.sin_addr));
                remove_player(&new_players, clientfd);
            }
        }
        
        /* Check which other socket descriptors have something ready to read.
         * The reason we iterate over the rset descriptors at the top level and
         * search through the two lists of clients each time is that it is
         * possible that a client will be removed in the middle of one of the
         * operations. This is also why we call break after handling the input.
         * If a client has been removed the loop variables may not longer be 
         * valid.
         */
        int cur_fd;  
        for(cur_fd = 0; cur_fd <= maxfd; cur_fd++) {
            if(FD_ISSET(cur_fd, &rset)) {
                char message[MAX_MSG];
                struct client *client = NULL;
                struct client *prev = NULL;
                int where = find_client(game.head, new_players, cur_fd, &client, &prev);
                if(where == 0) continue;

                int nbytes = read_from_client(client, message);

                // handle disconnections
                if(!client->is_connected) {
                    char exit_message[MAX_MSG];
                    sprintf(exit_message, "\r\n%s has just exited!\r\n", client->name);
                    printf("%s has just exited!\n", client->name);
                    broadcast(&game, exit_message, client, NULL);
                    if (where == 1) {
                        if(game.has_next_turn == client) advance_turn(&game);
                        remove_player(&game.head, client->fd);
                    } else if (where == 2) {
                        remove_player(&new_players, client->fd);
                    }
                    // at least one person in the game after exiting
                    if(game.head != NULL) announce_turn(&game, NULL);
                    continue;
                }

                // if we are acting on junk in message
                if (nbytes < 0) continue;

                // finally, if a stable connection
                if(where == 1) {
                    player_action(&game, client, message);
                } else if(where == 2) {
                    new_player_action(&game, &new_players, client, prev, message);
                }
            }
        }
    }
    return 0;
}
