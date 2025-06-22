#include "proxy_parse.h"
#include <iostream>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>       // for working with time
#include <sys/types.h>  // defines a set of fundamental system data types
#include <sys/socket.h> // defines a set of functions for working with sockets
#include <netinet/in.h> // define structures and macros related to Internet Protocol (IP) addresses and sockets
#include <netdb.h>      // for Host/service resolution
#include <arpa/inet.h>  // IP address conversion
#include <unistd.h>     // Unix system calls
#include <fcntl.h>      // File control operations
#include <sys/wait.h>   // Waiting for child processes
#include <memory>
#include <thread>
#include <mutex>
#include <semaphore.h>
#include <errno.h>

using namespace std;

constexpr int MAX_BYTES = 4096;                  // max allowed size of request/response
constexpr int MAX_CLIENTS = 400;                 // max number of client requests served at a time
constexpr int MAX_SIZE = 200 * (1 << 20);        // size of the cache (200 MB)
constexpr int MAX_ELEMENT_SIZE = 10 * (1 << 20); // max size of an element in cache (10 MB)

class CacheElement
{
public:
    string data;                                             // data stores response
    string url;                                              // url stores the request
    chrono::time_point<chrono::system_clock> lru_time_track; // lru_time_track stores the latest time the element is accessed
    shared_ptr<CacheElement> next;                           // pointer to next element

    CacheElement(const string &response_data, const string &request_url)
        : data(response_data), url(request_url), lru_time_track(chrono::system_clock::now()), next(nullptr) {}
};

class ProxyServer
{
private:
    int port_number = 8080;        // Default Port
    int proxy_socketId;            // socket descriptor of proxy server
    vector<thread> client_threads; // vector to store client threads
    sem_t semaphore;               // semaphore for limiting concurrent clients
    mutex cache_lock;              // mutex for cache synchronization
    shared_ptr<CacheElement> head; // pointer to the cache
    size_t cache_size = 0;         // cache_size denotes the current size of the cache

public:
    ProxyServer(int port = 8080) : port_number(port), head(nullptr)
    {
        sem_init(&semaphore, 0, MAX_CLIENTS);
    }

    ~ProxyServer()
    {
        if (proxy_socketId >= 0)
        {
            close(proxy_socketId);
        }
        // Wait for all threads to complete
        for (auto &thread : client_threads)
        {
            if (thread.joinable())
            {
                thread.join();
            }
        }
        sem_destroy(&semaphore);
    }

    void start()
    {
        cout << "Setting Proxy Server Port: " << port_number << endl;

        // Create proxy socket
        proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
        if (proxy_socketId < 0)
        {
            perror("Failed to create socket.\n");
            exit(1);
        }

        int reuse = 1;
        if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&reuse), sizeof(reuse)) < 0)
        {
            perror("setsockopt(SO_REUSEADDR) failed\n");
        }

        struct sockaddr_in server_addr;
        bzero(reinterpret_cast<char *>(&server_addr), sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_number);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        // Bind socket
        if (bind(proxy_socketId, reinterpret_cast<struct sockaddr *>(&server_addr), sizeof(server_addr)) < 0)
        {
            perror("Port is not free\n");
            exit(1);
        }

        cout << "Binding on port: " << port_number << endl;

        // Listen for connections
        if (listen(proxy_socketId, MAX_CLIENTS) < 0)
        {
            perror("Error while Listening!\n");
            exit(1);
        }

        cout << "Proxy server started and listening...\n";

        // Accept connections
        while (true)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_socketId = accept(proxy_socketId, reinterpret_cast<struct sockaddr *>(&client_addr), &client_len);

            if (client_socketId < 0)
            {
                cerr << "Error in Accepting connection!\n";
                continue;
            }

            // Get client info
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            cout << "Client connected from " << client_ip << ":" << ntohs(client_addr.sin_port) << endl;
        }
    }
};

int main(int argc, char *argv[])
{
    int port = 8080;

    if (argc == 2)
    {
        port = stoi(argv[1]);
    }
    else if (argc > 2)
    {
        cout << "Usage: " << argv[0] << " [port_number]" << endl;
        return 1;
    }

    try
    {
        ProxyServer server(port);
        server.start();
    }
    catch (const exception &e)
    {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}