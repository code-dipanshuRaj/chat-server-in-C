# Multi-Client Chat Application in C

## Overview
A simple yet powerful **multi-client chat system** implemented in C using **POSIX sockets**.  
The project demonstrates core concepts of **network programming**, including connection handling, concurrency, and message broadcasting.  

This was one of my first hands-on projects in networking, and it helped me build a deeper understanding of **TCP communication, multiplexing with `epoll`, and socket lifecycle management**.

---

## Features
- **Concurrent server**: Accepts multiple client connections simultaneously.
- **Broadcast messaging**: Any client can send messages visible to all connected clients.
- **Private messaging**: Clients can send direct messages to specific users (via a `/msg` command).
- **User registration**: Each client registers with a unique username.
- **Graceful disconnection**: Server detects and cleans up closed client sockets.
- **Socket utilities**: Wrapper functions for `send()` and `recv()` to ensure complete message transfer.

---

## Architecture
- **Server**
  - Uses `socket()`, `bind()`, `listen()`, and `accept()` for connection setup.
  - Manages multiple connections concurrently using `epoll` (I/O multiplexing).
  - Maintains a table of active clients (socket descriptors + usernames).
  - Routes messages (broadcast or private) to intended recipients.

- **Client**
  - Connects to server using `connect()`.
  - Handles two I/O streams: keyboard input and incoming server messages.
  - Simple command parsing (`/msg <user> <text>`) for private messaging.

- **Other clients do the same with a different name. Example commands:**
  -Broadcast: Hello everyone!.
  -Private message: /msg bob Hey Bob, are you there?.
  -List users: /list.
  -Quit: /quit.
---

## Interaction Interface

# Set name and broadcast messages
![1](./images/Screenshot%20(240).png)
# Send private messages
![2](./images/Screenshot%20(241).png)
# List users and graceful disconnection
![3](./images/Screenshot%20(242).png)

## How to Build
```bash
# Compile server
gcc -o server server.c -Wall

# Compile client
gcc -o client client.c -Wall
