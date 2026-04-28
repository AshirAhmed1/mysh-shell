# mysh

## Overview

`mysh` is a custom Unix-like shell written in C for the CSC209 systems programming project. It provides an interactive command-line interface with built-in commands, shell variables, pipes, background processes, process management, signal handling, and TCP-based networking commands.

This repository contains the final source version of `mysh`.

## Features

- Interactive shell prompt: `mysh$`
- Built-in shell commands
- Shell variable assignment and expansion
- Dynamic heap storage for shell variables
- File system navigation and listing
- File reading and word counting
- Pipes between commands
- External command execution using `fork` and `exec`
- Background process execution using `&`
- Process tracking with `ps`
- Process termination with `kill`
- Signal handling for `Ctrl+C`
- TCP server/client messaging support
- Non-blocking server with multiple connected clients
- Cleanup of child processes and dynamically allocated memory

## Technologies Used

- C
- GCC
- Makefile
- POSIX system calls
- Dynamic memory allocation
- Unix pipes
- Signals
- Process management
- TCP sockets
- `select`
- AddressSanitizer and UndefinedBehaviorSanitizer

## How to Run

1. Clone the repository:

```bash
git clone https://github.com/your-username/mysh.git
```

2. Go into the project directory:

```bash
cd mysh
```

3. Compile the shell:

```bash
make
```

4. Run the shell:

```bash
./mysh
```

5. Clean compiled files:

```bash
make clean
```

## Compilation

The provided `Makefile` builds an executable named:

```text
mysh
```

The Makefile uses the following compiler flags:

```text
-g -Wall -Wextra -Werror
-fsanitize=address,leak,object-size,bounds-strict,undefined
-fsanitize-address-use-after-scope
```

These flags help catch memory errors, undefined behavior, object-size errors, bounds errors, and other issues during development.

## Project Structure

```text
mysh/
│
├── Makefile
├── mysh.c
├── builtins.c
├── builtins.h
├── commands.c
├── commands.h
├── io_helpers.c
├── io_helpers.h
├── variables.c
└── variables.h
```

## Main Files

### `mysh.c`

Contains the main shell loop.

Responsibilities include:

- initializing the variable store
- initializing signal handling
- displaying the `mysh$` prompt
- reading user input
- enforcing the 128-character input limit
- handling `Ctrl+D`
- expanding variables
- detecting variable assignments
- spacing pipe/background operators
- parsing commands into jobs
- dispatching network commands
- executing jobs
- cleaning up processes and memory before exit

### `builtins.c` and `builtins.h`

Contain the built-in command table and the `echo` implementation.

The built-in command list includes:

```text
echo
cd
ls
cat
wc
ps
kill
```

The built-in function table maps command names to their corresponding C functions.

### `commands.c` and `commands.h`

Contain most of the shell execution logic.

Responsibilities include:

- implementing `cd`
- implementing `ls`
- implementing `cat`
- implementing `wc`
- implementing `ps`
- implementing `kill`
- parsing jobs and pipelines
- running foreground and background jobs
- managing pipes with `pipe` and `dup2`
- launching commands with `fork` and `execvp`
- tracking background processes
- printing background job completion messages
- handling signals
- implementing server/client networking commands

### `variables.c` and `variables.h`

Implement shell variable storage using a linked list.

Each variable is stored as a key-value pair:

```c
typedef struct VarNode {
    char *key;
    char *value;
    struct VarNode *next;
} VarNode;
```

Supported operations include:

- initializing the store
- setting variables
- replacing existing variable values
- retrieving variable values
- freeing all stored variables

### `io_helpers.c` and `io_helpers.h`

Contain helper functions for input/output.

Responsibilities include:

- writing messages to standard output
- writing errors to standard error
- reading user input
- tokenizing input by whitespace

## Supported Commands

```text
echo
cd
ls
cat
wc
ps
kill
exit
start-server
close-server
send
start-client
```

The shell also supports:

```text
|
&
```

for pipes and background execution.

## `echo`

Prints text to standard output.

Example:

```bash
mysh$ echo hello world
hello world
```

Extra empty tokens are skipped, and spaces are printed between arguments.

## `exit`

Exits the shell.

Example:

```bash
mysh$ exit
```

The shell also exits cleanly when input is closed with `Ctrl+D`.

## Shell Variables

`mysh` supports shell variable assignment and expansion.

Example:

```bash
mysh$ greeting=hello
mysh$ target=world
mysh$ echo $greeting $target
hello world
```

Variables can be redefined:

```bash
mysh$ greeting=hi
mysh$ echo $greeting
hi
```

Undefined variables expand to an empty string.

The implementation uses a dynamically allocated linked list and does not rely on `setenv` or `getenv`.

## Variable Expansion

Variable expansion is handled before command execution.

Example:

```bash
mysh$ cmd=echo
mysh$ msg=hello
mysh$ $cmd $msg
hello
```

Variables can also be expanded inside variable assignments:

```bash
mysh$ a=hello
mysh$ b=$a-world
mysh$ echo $b
hello-world
```

Expanded tokens are truncated to the maximum token size when necessary.

## Input Limit

The shell enforces a maximum input line length of:

```text
128 characters
```

If the input line is too long, the shell prints:

```text
ERROR: input line too long
```

The shell keeps running after this error.

## `cd`

Changes the current working directory.

Examples:

```bash
mysh$ cd src
mysh$ cd ..
mysh$ cd .
```

If no path is provided, `cd` changes to the user’s home directory.

The shell also supports extended dot-path navigation:

```text
.      current directory
..     one directory up
...    two directories up
....   three directories up
```

Invalid paths print:

```text
ERROR: Invalid path
```

Too many arguments print:

```text
ERROR: Too many arguments: cd takes a single path
```

## `ls`

Lists files and directories.

Examples:

```bash
mysh$ ls
mysh$ ls src
```

Supported options:

```text
--a
--f substring
--rec
--d depth
```

### Show Hidden Files

```bash
mysh$ ls --a
```

### Filter by Substring

```bash
mysh$ ls --f test
```

### Recursive Listing

```bash
mysh$ ls --rec
```

### Recursive Listing with Depth

```bash
mysh$ ls --rec --d 2
```

If `--d` is used without `--rec`, the command fails.

Invalid paths print:

```text
ERROR: Invalid path
```

Too many path arguments print:

```text
ERROR: Too many arguments: ls takes a single path
```

## `cat`

Displays the contents of a file.

Example:

```bash
mysh$ cat file.txt
```

`cat` can also read from standard input when used in a pipeline:

```bash
mysh$ cat file.txt | wc
```

If the file cannot be opened:

```text
ERROR: Cannot open file
```

If there is no input source:

```text
ERROR: No input source provided
```

## `wc`

Counts words, characters, and newlines.

Example:

```bash
mysh$ wc file.txt
word count 251
character count 1234
newline count 25
```

`wc` can also read from standard input through a pipe:

```bash
mysh$ cat file.txt | wc
```

The implementation counts characters and whitespace manually while reading file data from file descriptors.

## Pipes

`mysh` supports pipelines using `|`.

Example:

```bash
mysh$ cat file.txt | wc
```

Pipelines are parsed into jobs containing multiple commands. The shell creates pipes between commands and uses `dup2` to redirect standard input and standard output.

Built-in commands and external commands can both participate in pipelines.

## External Commands

If a command is not recognized as a built-in, `mysh` attempts to run it as an external program using:

```c
execvp
```

Example:

```bash
mysh$ sleep 5
```

The project does not use `system()`.

## Background Processes

Commands can be run in the background using `&`.

Example:

```bash
mysh$ sleep 20 &
[1] 12345
```

The shell tracks background processes and prints a completion message when they finish:

```text
[1]+ Done sleep 20
```

Background jobs are stored with job IDs, process IDs, command names, and remaining process counts.

## `ps`

Lists active processes launched by this shell.

Example:

```bash
mysh$ sleep 20 &
[1] 12345
mysh$ ps
sleep 12345
```

Only processes created by this shell are tracked and displayed.

## `kill`

Sends a signal to a process.

Usage:

```bash
mysh$ kill pid
mysh$ kill pid signum
```

If no signal is provided, the shell sends `SIGTERM`.

Invalid process:

```text
ERROR: The process does not exist
```

Invalid signal:

```text
ERROR: Invalid signal specified
```

## Signal Handling

`mysh` installs a custom `SIGINT` handler for `Ctrl+C`.

The shell uses different signal modes:

```text
SIGMODE_NONE
SIGMODE_IDLE
SIGMODE_BUILTIN
SIGMODE_WAITFG
```

When the shell is waiting for input, `Ctrl+C` does not terminate the shell.

When foreground child processes are running, `Ctrl+C` is forwarded to those foreground processes.

This allows the shell itself to remain alive while still allowing running commands to be interrupted.

## Network Commands

`mysh` includes TCP networking support for a simple chat-style system.

Supported networking commands:

```text
start-server
close-server
send
start-client
```

Networking is implemented using:

- `socket`
- `bind`
- `listen`
- `accept`
- `connect`
- `getaddrinfo`
- `select`
- `read`
- `write`

## `start-server`

Starts a background server on a given port.

Usage:

```bash
mysh$ start-server 5000
```

The server supports multiple clients and uses `select` to handle connections without blocking the whole server.

If no port is provided:

```text
ERROR: No port provided
```

## `close-server`

Stops the currently running server.

Usage:

```bash
mysh$ close-server
```

The shell sends a termination signal to the server process and waits for it to exit.

## `send`

Sends one message to a server.

Usage:

```bash
mysh$ send port-number hostname message
```

Example:

```bash
mysh$ send 5000 localhost hello world
```

The message is sent to the server and broadcast to connected interactive clients.

Messages must be shorter than 128 characters.

If no port is provided:

```text
ERROR: No port provided
```

If no hostname is provided:

```text
ERROR: No hostname provided
```

## `start-client`

Starts an interactive client connected to a server.

Usage:

```bash
mysh$ start-client port-number hostname
```

Example:

```bash
mysh$ start-client 5000 localhost
```

The client can send multiple messages through standard input until the input ends or the client is interrupted.

Each interactive client is assigned an ID by the server. Messages from clients are displayed in this format:

```text
client1: message
client2: message
```

The client also supports:

```text
\connected
```

This asks the server how many clients are currently connected.

## Error Handling

Errors are printed to standard error and begin with:

```text
ERROR:
```

Examples include:

```text
ERROR: input line too long
ERROR: Unknown command: command
ERROR: Invalid path
ERROR: Cannot open file
ERROR: No input source provided
ERROR: Too many arguments: cd takes a single path
ERROR: Too many arguments: ls takes a single path
ERROR: The process does not exist
ERROR: Invalid signal specified
ERROR: No port provided
ERROR: No hostname provided
ERROR: Builtin failed: command
```

The shell is designed to continue running after errors instead of crashing.

## Memory Management

The project uses dynamic memory allocation for:

- shell variables
- parsed commands
- jobs
- process tracking nodes
- background job strings
- network client buffers
- expanded input tokens

Before exiting, the shell cleans up:

- tracked child processes
- server process
- background process list
- background job list
- shell variable store

## Testing

To compile and run the shell manually:

```bash
make clean
make
./mysh
```

Example test commands:

```bash
echo hello world
x=hello
y=world
echo $x $y
ls
ls --a
ls --rec --d 1
cat file.txt
cat file.txt | wc
sleep 5 &
ps
kill <pid>
start-server 5000
send 5000 localhost hello
close-server
exit
```

## Important Restrictions

The project follows the course restrictions of avoiding:

```text
system()
setenv()
getenv()
```

Instead, it uses direct systems programming techniques such as:

- `fork`
- `execvp`
- `pipe`
- `dup2`
- `waitpid`
- `kill`
- `sigaction`
- `socket`
- `select`
- manual dynamic memory management

## Possible Future Improvements

- Add command history
- Add tab completion
- Add quote-aware parsing
- Add input/output redirection with `<`, `>`, and `>>`
- Add command chaining with `&&` and `||`
- Add foreground job control
- Add environment variable export support
- Add shell script execution
- Add stronger validation for networking edge cases

## Author

Ashir Ahmed

## Notes

This project was created for CSC209. It demonstrates low-level systems programming in C, including command parsing, process creation, pipes, signals, file system operations, dynamic memory management, and socket programming.
