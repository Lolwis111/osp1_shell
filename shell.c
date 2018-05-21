#include <stdio.h>
#include <libgen.h> 
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <stdbool.h>
#include "shell.h"

/* indicates if ctrl+c was hit 
 * marked as volatile as the value
 * gets updated in async interupts. */
volatile bool abortWait = false;
/* save the original handler for ctrl+c */
void (*handler)(int);
pid_t processes[MAX_PROCESSES]; /* keeps track of our processes */
size_t processCount = 0;        /* counts how many processes are running in the background */

void ctrlCHandler(int signo)
{
    /* if the user hit ctrl+c, we note that down */
    if(SIGINT == signo)
    {
        abortWait = true;
    }
}

/* checks if the given id is child of this shell */
bool isProcess(pid_t id)
{
    /* search for the id */
    for(int i = 0; i < MAX_PROCESSES; i++)
    {
        if(processes[i] == id)
        {
            return true;
        }
    }

    return false;
}

/* removes the id from the list of child processes */
void unregisterProcess(pid_t id)
{
    processCount--;
    /* check for the next free slot */
    for(int i = 0; i < MAX_PROCESSES; i++)
    {
        if(processes[i] == id) 
        {
            processes[i] = -1; 
            return;
        }
    }
}

/* registers a process in our structures */
void registerProcess(pid_t id)
{
    processCount++;
    /* check for the next free slot */
    for(int i = 0; i < MAX_PROCESSES; i++)
    {
        if(processes[i] == -1) 
        {
            processes[i] = id; 
            return;
        }
    }
}

/* removes trailing and leading whitespaces from the given string */
char *strtrim(char* str)
{
    /* from the beginning, skip all the whitespaces */
    while(isspace((unsigned char)*str))
    {
        str++;
    }

    /* if the string consists only of spaces we can end here */
    if(*str == 0)
    {
        return str;
    }

    /* now, starting from the end */
    char* end = str + strlen(str) - 1;
    /* skip all the trailing whitespaces */
    while(end > str && isspace((unsigned char)*end))
    {
        end--;
    }

    /* mark the first non-whitespace as end of string */
    *(end + 1) = 0;

    return str;
}

/* halts the termination until the process with the given pid terminated. */
void waitForPIDs(pid_t* pids, size_t argc)
{
    /* mark ids that are no childs of this shell as invalid */
    size_t abortedCount = 0;
    for(size_t i = 0; i < argc; i++)
    {
        if(!isProcess(pids[i]))
        {
            fprintf(stderr, "PID %d is not a child of this shell!\n", pids[i]);
            pids[i] = -1;
            abortedCount++;
        }
    }

    /* check if there even are valid arguments */
    if(argc == 0 || argc == abortedCount)
    {
        return;
    }

    /* reset Ctrl+C */
    abortWait = false;

    /* iterate through all the given pids until they all terminated */
    /* if the SIGINT handler registerd Ctrl+C we can return too */
    size_t i = 0;
    while(!abortWait)
    {
        /* -1 marks available slots, we dont have to wait on these */
        if(-1 != pids[i])
        {
            /* wait for the process with the given pid to terminate
             * BUT: WNOHANG makes waitpid() return no matter if
             * someone terminated or not. This makes the thread not hang.
             * return value 0 indicates that no one terminated. */
            int status;
            int ret = waitpid(pids[i], &status, WNOHANG);

            /* check for error */
            if(ret < 0)
            {
                fprintf(stderr, "%s\n", strerror(errno));
                return;
            }
            else if(ret > 0)
            {
                /* and print some data */
                printf("[%d] TERMINATED\n", pids[i]);
                printf("[%d] EXIT STATUS: %i\n", pids[i], WEXITSTATUS(status));
                printf("[%d] NORMAL TERMIATION: %s\n", pids[i], (WIFEXITED(status) ? "yes" : "no"));
                if(WIFSTOPPED(status))
                {
                    printf("[%d] STOP: %d\n", pids[i], WSTOPSIG(status));
                }

                if(WIFSIGNALED(status))
                {
                    printf("[%d] SIGNAL: %s\n", pids[i], strsignal(WTERMSIG(status)));
#ifdef WCOREDUMP
                    printf("[%d] CORE DUMP: %s\n", pids[i], (WCOREDUMP(status) ? "yes" : "no"));
#endif
                }
            
                /* remove this process from the shell */
                unregisterProcess(pids[i]);
                abortedCount++; /* count how many we aborted to know when this loop should end */

                /* if we worked through all the pids we can return */
                if(abortedCount == argc)
                {
                    return;
                }

                /* go to the next pid */
                i++;

                /* this loop iterates through all the pids all over again */
                if(i == argc)
                {
                    i = 0;
                }
            }
            // else: if waitpid returns 0, it means that no child terminated, so we can ignore that
        }
    }
}

/* waits for all the running background tasks to finish */
void killall()
{
    for(size_t i = 0; i < MAX_PROCESSES; i++)
    {
        /* check if the slot needs attention */
        if(processes[i] != 0)
        {
            /* if yes, wait for it to finish */
            kill(processes[i], SIGTERM);
        }
    }
}

/* frees a paramter block */
void freePrograms(program_t* params)
{
    if(params == NULL || params->args == NULL) 
    {
        return;
    }

    /* free the strings */
    for(size_t i = 0; i < params->argc; i++)
    {
        if(params->args[i] != NULL)
        {
            free(params->args[i]);
            params->args[i] = NULL;
        }
    }
    /* and the string array itself */
    free(params->args);
    params->args = NULL;
}


/* frees a command structure */
void freeCommand(command_t* command)
{
    if(command == NULL || command->programs == NULL)
    {
        return;
    }

    /* free all the programs */
    for(int i = 0; i < command->count; i++)
    {
        freePrograms(&command->programs[i]);
    }


    free(command->programs);
    /* reset the data */
    command->programs = NULL;
    command->count = 0;
    command->malloced = 0;
    command->background = false;
}

/* launches the given programms and opens a pipe
   from program1 to program2 */
void launchProgramWithPipe(command_t* command)
{
    /* the two processes */
    pid_t process1 = 0, process2 = 0;

    process1 = fork();

    if(process1 < 0)
    {
        /* if we reach this path the fork has failed
         * and there is no point to continue the execution */
        fprintf(stderr, "%s\n", strerror(errno));
        fflush(stderr);

        freeCommand(command);

        exit(EXIT_FAILURE);
    }
    else if(process1 == 0) /* fork once for the first program */
    {
        /* background processes should ingore Ctrl+C,
           everyone else gets the default handler  */
        signal(SIGINT, command->background ? SIG_IGN : handler);

        /* start up a pipe and check if that worked */
        int pipefds[2];
        if(0 > pipe(pipefds))
        {
            fprintf(stderr, "%s\n", strerror(errno));
            fflush(stderr);

            /* free the process information blocks */
            freeCommand(command);

            exit(EXIT_FAILURE);
        }

        /* fork the child again, as pipe() can
         * only be applied to a parent and its child */
        process2 = fork();
        if(process2 == 0)
        {
            /* the first process needs readjusting on the stdout stream */
            close(pipefds[0]);
            dup2(pipefds[1], STDOUT_FD); /* set up the pipe */
            close(pipefds[1]);

            /* ready for launch! */
            execvp(command->programs[0].args[0], command->programs[0].args);
        }
        else if(process2 > 0)
        {
            /* the second process needs readjusting on the stdin stream */
            close(pipefds[1]);
            dup2(pipefds[0], STDIN_FD); /* set up the pipe */
            close(pipefds[0]);

            /* ready for launch! */
            execvp(command->programs[1].args[0], command->programs[1].args);
        }

        fprintf(stderr, "%s\n", strerror(errno));
        fflush(stderr);

        /* free the process information blocks */
        freeCommand(command);

        exit(EXIT_FAILURE);
    }
    else
    {
        if(!command->background)
        {
            /* wait for both processes to finish */
            int status;
            waitpid(process1, &status, 0);
        }
        else
        {
            /* register the process */
            registerProcess(process1);
            
            /* print the pid for background processes */
            printf("\n[%d]\n", (int)process1);
        }

        freeCommand(command);
    }
}

/* launches the given programs. The background parameter
   indicates if the execution should be halted until
   the process finishes. */
void launchProgram(command_t* command)
{
    /* fork us */
    pid_t process = fork();
    
    /* did that work? */
    if (process < 0)
    {
        fprintf(stderr, "%s\n", strerror(errno));
        fflush(stderr);

        freeCommand(command);

        exit(EXIT_FAILURE);
    }
    /* the child has to execute the requested program */
    else if (process == 0)
    { 
        /* background processes should ingore Ctrl+C,
           everyone else gets the default handler  */
        signal(SIGINT, command->background ? SIG_IGN : handler);

        execvp(command->programs[0].args[0], command->programs[0].args);

        /* did an error occur? */
        fprintf(stderr, "%s\n", strerror(errno));
        fflush(stderr);

        freeCommand(command);

        exit(EXIT_FAILURE);
    }
    else
    { 
        /* wait only for programs that are not in the 
           background (no ampersand operator) */
        if(!command->background)
        {
            int status;
            waitpid(process, &status, 0);
        }
        else
        {
            /* register the process */
            registerProcess(process);

            /* print the pid for background processes */
            printf("\n[%d]\n", (int)process);
        }

        /* free the process information block */
        freeCommand(command);
    }
}

/* takes the given string and splits it into the program and
   the given arguments. The data is saved in the params structure.
   One should free params after usage, as it gets malloc'd
   in the function. */
bool parseProgram(program_t* params, char* programString)
{
    /* set up the parameter structure */
    params->argc = 0;
    params->malloced = 1;
    params->args = __malloc(char*, params->malloced);

    if(NULL == params->args)
    {
        fprintf(stderr, "%s\n", strerror(errno));
        return false;
    }

    /* split the string at the spaces */
    char* token;
    while((token = strsep(&programString, " ")) != NULL)
    {
        /* remove leading/trailing whitespaces */
        token = strtrim(token);

        if(strlen(token) > 0)
        {
            /* copy the argument into the args array 
               (and allocate memory for the string) */
            params->args[params->argc] = __malloc(char, strlen(token) + 1);

            if(NULL == params->args[params->argc])
            {
                fprintf(stderr, "%s\n", strerror(errno));
                return false;
            }

            strcpy(params->args[params->argc], token);
            params->argc++;

            /* create more space to store more arguments */
            if(params->argc == params->malloced)
            {
                params->malloced++;
                char** temp = __realloc(params->args, char*, params->malloced);

                if(NULL == temp)
                {
                    fprintf(stderr, "%s\n", strerror(errno));
                    free(params->args);
                    return false;
                }
                params->args = temp;
            }
        }
    }
    params->args[params->argc] = NULL;
    return true;
}

/* processes a command (detects pipes and programs/arguments) */
bool parseCommand(char* commandStr, command_t *command)
{
    command->background = false;
    command->count = 0;
    command->malloced = 2;
    command->programs = __malloc(program_t, command->malloced);

    if(NULL == command->programs)
    {
        return false;
    }

    int length = strlen(commandStr);
    if('&' == commandStr[length - 1])
    {
        /* the very last character & means its an background task */
        command->background = true;
        commandStr[length - 1] = '\0';
    }

    if(command->background && MAX_PROCESSES == processCount)
    {
        fputs("Maximum number of processes reached!\n", stderr);
        return false;
    }

    /* split the string at the pipe operator to 
     * get the programs and their arguments */
    char *token;
    while((token = strsep(&commandStr, "|")) != NULL)
    {
        /* parse each program+arguments */
        if(!parseProgram(&command->programs[command->count], strtrim(token)))
        {
            return false;
        }

        command->count++;

        if(command->count == command->malloced)
        {
            command->malloced += 2;
            program_t* temp = __realloc(command->programs, program_t, command->malloced);

            if(NULL == temp)
            {
                fprintf(stderr, "%s\n", strerror(errno));
                free(command->programs);
                return false;
            }
            command->programs = temp;
        }
    }

    return true;
}

int main(void)
{
    /* install a new handler for Ctrl+C 
       but also save the old handler 
       for background children */
    handler = signal(SIGINT, ctrlCHandler);

    /* set all process ids to zero
       zero indicates that the slot is available */
    for(size_t i = 0; i < MAX_PROCESSES; i++)
    {
        processes[i] = -1;
    }

    /* inputBuffer is where the user input is stored */
    char inputBuffer[INPUT_BUFFER_LENGTH];

    while(1)
    {
        char *cwd = __malloc(char, PATH_LENGTH);
        if(NULL == cwd)
        {
            fprintf(stderr, "%s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        /* get the working directory for our process */
        if(getcwd(cwd, PATH_LENGTH) == NULL)
        {
            free(cwd);
            fprintf(stderr, "%s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }

        /* print a prompt */
        printf("%s /> ", basename(cwd));
        free(cwd);

        /* (try) to read a command from the user */
        if(NULL == fgets(inputBuffer, INPUT_BUFFER_LENGTH, stdin))
        {
            /* this indicates that EOF was entered
               bash exits on EOF so we do too */
            putc('\n', stdout);
            exit(EXIT_SUCCESS);
        }

        /* delete the trailing newline character that fgets inserted */
        inputBuffer[strlen(inputBuffer) - 1] = 0x00;
        char* input = strtrim(inputBuffer);

        /* check if the input is empty */
        if(strlen(inputBuffer) == 0)
            continue;

        /* this where we put the parsed command */
        command_t command;

        /* parse the input into a command (split into programs and arguments) */
        if(!parseCommand(input, &command))
        {
            freeCommand(&command);
            exit(EXIT_FAILURE);
        }

        if(command.count == 1)
        {
            /* did the user type exit? */
            if(0 == strcmp(command.programs[0].args[0], "exit"))
            {
                /* wait for all background processes to finish */
                killall();
                freeCommand(&command);
                exit(EXIT_SUCCESS);
            }
            /* did the user type cd? */
            else if(0 == strncmp(command.programs[0].args[0], "cd", 2))
            {
                /* cd only handles one argument */
                if(command.programs[0].argc > 2)
                {
                    fputs("Too many arguments!\n", stderr);
                }
                else if(command.programs[0].argc == 1)
                {
                    fputs("Too few arguments!\n", stderr);
                }
                else
                { 
                    /* change the directory, the path is at least
                     * one space after the command, so we start at
                     * offset 3. */
                    if(chdir(command.programs[0].args[1]) == -1)
                    {
                        fprintf(stderr, "%s\n", strerror(errno));
                    }
                }

                freeCommand(&command);
            }
            /* did the user type wait? */
            else if(0 == strncmp(command.programs[0].args[0], "wait", 4))
            {
                /* allocate some memory for all the pids */
                size_t argc = command.programs[0].argc;
                pid_t* pids = __malloc(pid_t, argc - 1);

                for(size_t i = 1; i < argc; i++)
                {
                    /* try to parse all the arguments into pids */
                    char *ptr;
                    pid_t pid = (pid_t)strtol(command.programs[0].args[i], &ptr, 10);

                    if(ptr == command.programs[0].args[1] || errno == ERANGE) /* check for errors */
                    {
                        fprintf(stderr, "'%s' is not a valid process id!\n", command.programs[0].args[1]);
                    }
                    else
                    {
                        pids[i - 1] = pid;
                    }
                }

                /* start waiting for all of them */
                waitForPIDs(pids, argc - 1);

                free(pids);
                freeCommand(&command);
            }
            else
            {
                launchProgram(&command);
            }
        }
        else if(command.count == 2)
        {
            launchProgramWithPipe(&command);
            // free(command.programs);
        }
        else
        {
            fprintf(stderr, "A pipe of length %zu was detected!\nThis shell only supports pipes of length 2!\n[execution aborted]\n", command.count);
            freeCommand(&command);
        }
    }
}
