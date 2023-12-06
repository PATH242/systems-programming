# Systems Programming
This is a repository for learning about systems programming.

### Lab 1
C program that takes file names with numbers, sorts each of them individually with quick sort, and then uses merge sort to merge them. The result will always be stored in results.txt. There's a checker script in ./lab1 that can be used to check numbers have been sorted correctly.

Please compile it with:
```
gcc -Wextra -Werror -Wall -Wno-gnu-folding-constant -ldl -rdynamic ./lab1/solution.c ./lab1/libcoro.c ./utils/heap_help.c
```
And run it with the files you want using:\
*HHREPORT=l will enable memory leak checks.* 
```
HHREPORT=l ./a.out ./lab1/test1.txt ./lab1/test2.txt ./lab1/test3.txt ./lab1/test4.txt ./lab1/test5.txt ./lab1/test6.txt 
```

To test results, please use checker.py.
```
python3 ./lab1/checker.py -f result.txt
```

### Lab 2
**Simplified version of a command line.**

Please compile it with
```
gcc -Wextra -Werror -Wall -Wno-gnu-folding-constant ./lab2/solution.c ./lab2/parser.c
```
and run it using 
```
./a.out 
```
then start using it as a terminal. If you need to exit, type exit.
Command line also supports:
⇥   and, or operations: &&, ||
⇥   pipes |
⇥   background operations &
### Lab 3
**Simple file system** that supports:
⇥   *Open via FD.*
⇥   *Read via FD.*
⇥   *Write via FD.*
⇥   *Close FD.*
⇥   *Delete via file name.*
and permission flags.
Deleted files remain accessible through the fds opened on them before deletion, and are fully deleted after these fds referencing them close.

To run on tests:
```
cd lab3 && make && ./a.out
```

### Lab 4
**Thread pool**

To run on tests:
```
cd lab4 && make && ./a.out
```

### Lab 5
**Game Lobby chat**
supports
⇥   *Multiple clients*
⇥   *Long messages*
⇥   *Incomplete messages' buffering*
⇥   *Sending messages in batches*
⇥   *Client sends its name once and it is attached to all its messages*
### Acknowledgments

This repository is built on the foundation of [sysprog](https://github.com/Gerold103/sysprog/tree/master) by [Vladislav Shpilevoy]. It is a systems programming course repo and tasks' descriptions and solutions' templates are authored by him.