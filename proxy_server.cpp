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
        // ProxyServer server(port);
        // server.start();
    }
    catch (const exception &e)
    {
        cerr << "Error: " << e.what() << endl;
        return 1;
    }

    return 0;
}