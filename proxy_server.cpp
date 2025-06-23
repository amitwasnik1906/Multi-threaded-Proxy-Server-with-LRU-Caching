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

// O(1) LRU Cache Implementation with normalized keys
class LRUCache
{
private:
    class Node
    {
    public:
        string cache_key; // Normalized cache key
        string data;      // Server response data
        Node *next;
        Node *prev;
        size_t size;

        Node(const string &_key, const string &_data)
            : cache_key(_key), data(_data), next(nullptr), prev(nullptr)
        {
            size = cache_key.length() + data.length() + sizeof(Node);
        }
    };

    Node *head;
    Node *tail;
    size_t capacity;
    size_t current_size;
    unordered_map<string, Node *> cache_map;
    mutable mutex cache_mutex;

    void addToHead(Node *node)
    {
        node->prev = head;
        node->next = head->next;
        head->next->prev = node;
        head->next = node;
    }

    void removeNode(Node *node)
    {
        node->prev->next = node->next;
        node->next->prev = node->prev;
    }

    Node *removeTail()
    {
        Node *last_node = tail->prev;
        removeNode(last_node);
        return last_node;
    }

    void moveToHead(Node *node)
    {
        removeNode(node);
        addToHead(node);
    }

public:
    LRUCache(size_t _capacity) : capacity(_capacity), current_size(0)
    {
        head = new Node("", "");
        tail = new Node("", "");
        head->next = tail;
        tail->prev = head;
    }

    ~LRUCache()
    {
        lock_guard<mutex> lock(cache_mutex);
        Node *current = head;
        while (current)
        {
            Node *next = current->next;
            delete current;
            current = next;
        }
    }

    // O(1) get operation
    string get(const string &cache_key)
    {
        lock_guard<mutex> lock(cache_mutex);

        auto it = cache_map.find(cache_key);
        if (it == cache_map.end())
        {
            return ""; // Not found
        }

        Node *node = it->second;
        moveToHead(node);

        cout << "Cache HIT for key: " << cache_key << endl;
        return node->data;
    }

    // O(1) put operation
    bool put(const string &cache_key, const string &data)
    {
        lock_guard<mutex> lock(cache_mutex);

        size_t element_size = cache_key.length() + data.length() + sizeof(Node);

        // Check if element is too large
        if (element_size > MAX_ELEMENT_SIZE)
        {
            cout << "Element too large for cache" << endl;
            return false;
        }

        auto it = cache_map.find(cache_key);
        if (it != cache_map.end()) // Already exits in cache_map
        {
            // Update existing node
            Node *node = it->second;
            current_size -= node->size;
            node->data = data;
            node->size = element_size;
            current_size += element_size;
            moveToHead(node);
            cout << "Cache UPDATED for key: " << cache_key << endl;
            return true;
        }

        // Remove nodes until we have space
        while (current_size + element_size > capacity && cache_map.size() > 0)
        {
            Node *last_node = removeTail();
            cache_map.erase(last_node->cache_key);
            current_size -= last_node->size;
            cout << "Cache EVICTED key: " << last_node->cache_key << endl;
            delete last_node;
        }

        // Add new node
        Node *new_node = new Node(cache_key, data);
        addToHead(new_node);
        cache_map[cache_key] = new_node;
        current_size += element_size;

        cout << "Cache ADDED key: " << cache_key << endl;
        cout << "Cache size: " << cache_map.size() << " elements, "
             << current_size / (1024 * 1024) << " MB" << endl;

        return true;
    }

    size_t size() const
    {
        lock_guard<mutex> lock(cache_mutex);
        return cache_map.size();
    }

    size_t getCurrentSize() const
    {
        lock_guard<mutex> lock(cache_mutex);
        return current_size;
    }
};

class ProxyServer
{
private:
    int port_number = 8080; // Default Port
    int proxy_socketId;     // socket descriptor of proxy server
    sem_t semaphore;        // semaphore for limiting concurrent clients
    LRUCache *cache;        // O(1) LRU Cache

public:
    ProxyServer(int port = 8080) : port_number(port)
    {
        sem_init(&semaphore, 0, MAX_CLIENTS);
        this->cache = new LRUCache(MAX_SIZE);
    }

    ~ProxyServer()
    {
        if (proxy_socketId >= 0)
        {
            close(proxy_socketId);
        }
        sem_destroy(&semaphore);
        delete cache;
    }

    int sendErrorMessage(int socket, int status_code)
    {
        string str;
        char currentTime[50];
        time_t now = time(nullptr);

        struct tm *data = gmtime(&now);
        strftime(currentTime, sizeof(currentTime), "%a, %d %b %Y %H:%M:%S %Z", data);

        switch (status_code)
        {
        case 400:
            str = "HTTP/1.1 400 Bad Request\r\nContent-Length: 95\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " +
                  string(currentTime) + "\r\nServer: ProxyServer/1.0\r\n\r\n<HTML><HEAD><TITLE>400 Bad Request</TITLE></HEAD>\n<BODY><H1>400 Bad Request</H1>\n</BODY></HTML>";
            cout << "400 Bad Request\n";
            break;
        case 403:
            str = "HTTP/1.1 403 Forbidden\r\nContent-Length: 112\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: " +
                  string(currentTime) + "\r\nServer: ProxyServer/1.0\r\n\r\n<HTML><HEAD><TITLE>403 Forbidden</TITLE></HEAD>\n<BODY><H1>403 Forbidden</H1><br>Permission Denied\n</BODY></HTML>";
            cout << "403 Forbidden\n";
            break;
        case 404:
            str = "HTTP/1.1 404 Not Found\r\nContent-Length: 91\r\nContent-Type: text/html\r\nConnection: keep-alive\r\nDate: " +
                  string(currentTime) + "\r\nServer: ProxyServer/1.0\r\n\r\n<HTML><HEAD><TITLE>404 Not Found</TITLE></HEAD>\n<BODY><H1>404 Not Found</H1>\n</BODY></HTML>";
            cout << "404 Not Found\n";
            break;
        case 500:
            str = "HTTP/1.1 500 Internal Server Error\r\nContent-Length: 115\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " +
                  string(currentTime) + "\r\nServer: ProxyServer/1.0\r\n\r\n<HTML><HEAD><TITLE>500 Internal Server Error</TITLE></HEAD>\n<BODY><H1>500 Internal Server Error</H1>\n</BODY></HTML>";
            break;
        case 501:
            str = "HTTP/1.1 501 Not Implemented\r\nContent-Length: 103\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " +
                  string(currentTime) + "\r\nServer: ProxyServer/1.0\r\n\r\n<HTML><HEAD><TITLE>501 Not Implemented</TITLE></HEAD>\n<BODY><H1>501 Not Implemented</H1>\n</BODY></HTML>";
            cout << "501 Not Implemented\n";
            break;
        case 505:
            str = "HTTP/1.1 505 HTTP Version Not Supported\r\nContent-Length: 125\r\nConnection: keep-alive\r\nContent-Type: text/html\r\nDate: " +
                  string(currentTime) + "\r\nServer: ProxyServer/1.0\r\n\r\n<HTML><HEAD><TITLE>505 HTTP Version Not Supported</TITLE></HEAD>\n<BODY><H1>505 HTTP Version Not Supported</H1>\n</BODY></HTML>";
            cout << "505 HTTP Version Not Supported\n";
            break;
        default:
            return -1;
        }

        send(socket, str.c_str(), str.length(), 0);
        return 1;
    }

    void handleClient(int clientSocket)
    {
        sem_wait(&semaphore);

        vector<char> buffer(MAX_BYTES);
        int bytes_received = recv(clientSocket, buffer.data(), MAX_BYTES - 1, 0);

        while (bytes_received > 0)
        {
            buffer[bytes_received] = '\0';
            string buffer_str(buffer.data());

            // Check if we have complete HTTP request
            if (buffer_str.find("\r\n\r\n") != string::npos)
            {
                break;
            }

            int additional_bytes = recv(clientSocket, buffer.data() + bytes_received,
                                        MAX_BYTES - bytes_received - 1, 0);
            if (additional_bytes <= 0)
                break;
            bytes_received += additional_bytes;
        }

        if (bytes_received <= 0)
        {
            if (bytes_received < 0)
            {
                perror("Error in receiving from client.\n");
            }
            else
            {
                cout << "Client disconnected!\n";
            }
            shutdown(clientSocket, SHUT_RDWR);
            close(clientSocket);
            sem_post(&semaphore);
            return;
        }

        buffer[bytes_received] = '\0';

        // Parse request
        ParsedRequest *request = ParsedRequest_create();

        if (ParsedRequest_parse(request, buffer.data(), bytes_received) < 0)
        {
            cout << "Parsing failed\n";
            sendErrorMessage(clientSocket, 400);
        }
        else
        {
            // Process request
            
        }

        ParsedRequest_destroy(request);
        shutdown(clientSocket, SHUT_RDWR);
        close(clientSocket);
        sem_post(&semaphore);
    }

    void start()
    {
        cout << "Setting Proxy Server Port: " << port_number << endl;
        cout << "Cache Configuration: Max Size = " << MAX_SIZE / (1024 * 1024)
             << " MB, Max Element Size = " << MAX_ELEMENT_SIZE / (1024 * 1024) << " MB" << endl;

        // Create proxy socket
        proxy_socketId = socket(AF_INET, SOCK_STREAM, 0);
        if (proxy_socketId < 0)
        {
            perror("Failed to create socket.\n");
            exit(1);
        }

        int reuse = 1;
        if (setsockopt(proxy_socketId, SOL_SOCKET, SO_REUSEADDR,
                       reinterpret_cast<const char *>(&reuse), sizeof(reuse)) < 0)
        {
            perror("setsockopt(SO_REUSEADDR) failed\n");
        }

        struct sockaddr_in server_addr;
        bzero(reinterpret_cast<char *>(&server_addr), sizeof(server_addr));
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port_number);
        server_addr.sin_addr.s_addr = INADDR_ANY;

        // Bind socket
        if (bind(proxy_socketId, reinterpret_cast<struct sockaddr *>(&server_addr),
                 sizeof(server_addr)) < 0)
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

        cout << "Proxy server started and listening with O(1) LRU Cache...\n";

        // Accept connections
        while (true)
        {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);

            int client_socketId = accept(proxy_socketId,
                                         reinterpret_cast<struct sockaddr *>(&client_addr),
                                         &client_len);

            if (client_socketId < 0)
            {
                cerr << "Error in Accepting connection!\n";
                continue;
            }

            // Get client info
            char client_ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
            cout << "\nClient connected from " << client_ip
                 << ":" << ntohs(client_addr.sin_port) << endl;

            // Handle client in a separate thread
            thread([this, client_socketId]()
                   { this->handleClient(client_socketId); })
                .detach();
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