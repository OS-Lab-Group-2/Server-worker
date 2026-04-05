# Distributed Task Execution System (DTE)

A minimal, production-aware distributed execution framework written in **pure C**. It uses Linux TCP sockets, `fork()`/`exec()`, and `/proc/loadavg` for dynamic load-balanced scheduling.

---

## 1. Quick Start (Local Execution)

Follow these steps to run the system on your own machine using `127.0.0.1`.

### Step 1: Build the Project
Compile the Server and Worker binaries.
```bash
make all
```

### Step 2: Start the Server (Interactive Console)
Open a terminal and start the server. It will display its IP address and wait for commands.
```bash
./server
```

### Step 3: Register a Worker
Open a **second terminal** and start a worker, passing the server's IP address (e.g., `127.0.0.1` for local testing).
```bash
./worker 127.0.0.1
```
The server terminal will notify you that a new worker has registered.

### Step 4: Dispatch a Task
In the **first terminal** (the Server), type the name of the task you want to execute:
```bash
dte> task_example.c
```

---

## 2. How It Works (Workflow)

The system follows a specific sequence of events to ensure tasks are executed on the best available machine.

### The Protocol "Handshake"

| Step | Server (Coordinator) | Worker |
| :--- | :--- | :--- |
| **1** | **Starts & Listens:** Waits for registrations | *Starts & connects* to Server |
| **2** | **Registers Worker:** Adds to internal pool | ← Sends `MSG_REGISTER` (every 5s) |
| **3** | *[User types **`task.c`**]* Compiles → Binary | *Listening on ports 9100/9101* |
| **4** | **Query Load:** "How busy are you?" → | ← Reads `/proc/loadavg` and responds |
| **5** | Picks best worker & connects | *Forking child to handle execution* |
| **6** | **Send Task:** Streams binary data → | ← ACKs receipt & saves to `/tmp` |
| **7** | *Waiting for results...* | **Executes:** Runs binary & captures output |
| **8** | **Receive Result:** Displays output | ← Sends captured text & deletes binary |

### Entity Roles
- **Server (Coordinator):** The brain. It provides an interactive console, accepts worker registrations, compiles code, checks worker health/load, and dispatches binaries.
- **Worker:** The muscle. It registers with the Server in the background, reports its CPU load, receives binaries, and executes them in an isolated process.

---

## 3. Project Structure

```text
.
├── server.c          # Server source (Dispatcher)
├── worker.c          # Worker daemon source (Executor)
├── protocol.h        # Shared wire-format definitions & I/O helpers
├── task_example.c    # Sample task (Fibonacci calculation)
├── Makefile          # Build system (make all, make clean)
└── README.md         # This documentation
```

---

## 4. Technical Reference

### Network Ports
- **9100 (Exec Port):** Used for binary transfer and receiving execution results.
- **9101 (Load Port):** Used for quick load-balancing queries.
- **9102 (Register Port):** Used by the Server to receive automatic `MSG_REGISTER` pings from Workers.

### Architecture Highlights
- **Concurrency:** Worker handles multiple simultaneous clients using `fork()`.
- **Load Balancing:** Server always selects the worker with the lowest **1-minute load average**.
- **Execution:** Tasks are run via `execv()` with stdout/stderr redirected into a pipe for capture.
- **Cleanup:** Temporary binaries are unlinked immediately after execution to save disk space.

---

## 5. Security Considerations

> [!WARNING]  
> This system is designed for **trusted LAN environments** or local testing. It has **no authentication or encryption**.

| Risk | Mitigation |
| :--- | :--- |
| **Arbitrary Execution** | Run `worker` as a low-privilege user; Use firewalls to restrict access. |
| **Plaintext Transport** | Avoid running on public Wi-Fi; Use a VPN or SSH tunnel for remote work. |
| **No Authentication** | Only expose ports 9100/9101 to trusted internal IP addresses. |

---

## 6. Advanced Usage (LAN Setup)

To run this system across multiple physical machines, follow these steps:

### Step 1: Identify IP Addresses
On each **Worker (Device 2, 3...)**, find the local IP address:
```bash
ip addr show | grep "inet "
# Look for 192.168.x.x
```

### Step 2: Configure Firewalls
On each Worker, you must allow incoming TCP traffic on ports **9100** and **9101**.
On the Server, you must allow incoming TCP traffic on port **9102**.
```bash
# Ubuntu/Debian (UFW) - On Worker
sudo ufw allow 9100/tcp
sudo ufw allow 9101/tcp

# Ubuntu/Debian (UFW) - On Server
sudo ufw allow 9102/tcp
```

### Step 3: Start the Server
On the **Server (Device 1)**, launch `server`. Note the IP address it prints.
```bash
./server
# Example Output: [SERVER] Local IP address: 192.168.1.10
```

### Step 4: Connect Workers
Run `./worker` on every underlying physical machine, pointing them to the Server IP.
```bash
./worker 192.168.1.10
```
Workers will automatically ping the server every 5 seconds to maintain their registration. You can now use the `workers` command in the `dte>` console to verify they are connected.

### Troubleshooting
- **Ping Check:** Ensure `ping [Worker_IP]` works from the Server.
- **Connection Timeout:** Usually caused by a **firewall** blocking ports 9100/9101 on the worker side.
- **Connection Refused:** Ensure `worker` is actually running on the worker.

---

## 7. Makefile Commands
- `make all`: Build both binaries.
- `make clean`: Remove binaries and build artifacts.
- `make install`: Install as `dte_coordinator` and `dte_worker` to `/usr/local/bin` (requires sudo).
