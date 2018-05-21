#ifndef _SHELL_H_
#define _SHELL_H_

#define INPUT_BUFFER_LENGTH 512 /* allow the user 512 characters of command */
#define MAX_PROCESSES 8 /* allow 8 background processes */
#define STDIN_FD 0 /* cool names for the streams */
#define STDOUT_FD 1
#define PATH_LENGTH (FILENAME_MAX > 4096 ? 4096 : FILENAME_MAX)

/* some convenient typesafe memory allocations */
#define __malloc(type, amount) (type*)malloc(sizeof(type) * (amount))
#define __realloc(ptr, type, amount) (type*)realloc(ptr, sizeof(type) * (amount))

/* This struct holds the data to start
 * a process. */
typedef struct program_s {
    char** args;     /* the arguments itself [note: args[0] is the program itself */
    size_t argc;     /* how many args we actually use */
    size_t malloced; /* how many args we have malloced */
} program_t;

/* This struct holds the data of the
 * current command */
typedef struct command_s {
	program_t *programs; /* all the programs assoiated with the command */
	size_t count;		 /* how many programs there are */
	size_t malloced;	 /* how much space we have */
	bool background;     /* is this command a background task? */
} command_t;


#endif