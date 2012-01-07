#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <stdint.h>
#include <assert.h>
#include <signal.h>
#include <setjmp.h>
#include <string>
#include <map>
#include <boost/filesystem.hpp>
#include "mongoose.h"
using namespace boost::filesystem;


// functions for logging
bool verbose = false;
void log_printf(const char *format, ...)
{
    if (verbose)
    {
        va_list args;
        va_start(args, format);
        vfprintf(stdout, format, args);
        va_end(args);
    }
}


// functions for long jumping
static jmp_buf exit_env;
static void sig_hand(int code)
{
    longjmp(exit_env, code);
}


// http handling
std::string port = "8080";
std::map<uint64_t, path> mappings;
void handle_get(mg_connection *conn,
                const mg_request_info *request)
{
    const char *response_status = NULL;
    uint64_t uuid = atoll(request->uri + 1);
    log_printf("uuid requested: %llu\n", uuid);

    if (mappings.find(uuid) == mappings.end())
    {
        log_printf("uuid doesn't exist\n");
        response_status = "404 Not Found";
    }
    else
    {
        path p = mappings[uuid];
        if (!exists(p) || is_directory(p))
        {
            log_printf("file no longer exists: %s\n", p.c_str());
            response_status = "410 Gone";
        }
        else
        {
            FILE *file = fopen(p.c_str(), "r");
            if (file == NULL)
            {
                log_printf("failed to open file: %s\n", p.c_str());
                response_status = "410 Gone";
            }
            else
            {
                fclose(file);

                mg_send_file(conn, p.c_str());
                mappings.erase(uuid);
                return;
            }
        }
    }

    log_printf("responded with %s\n", response_status);
    mg_printf(conn, "HTTP/1.1 %s\r\n"
              "Content-Type: text/plain\r\n\r\n",
              response_status);
}

void handle_post(mg_connection *conn,
                 const mg_request_info *request)
{
    const char *response_status = NULL;
    char response_content[256] = "";

    path p(request->uri);
    if (exists(p))
    {
        if (is_directory(p))
        {
            log_printf("error: is directory\n");
            response_status = "400 Bad Request";
        }
        else
        {
            FILE *file = fopen(p.c_str(), "r");
            if (file == NULL)
            {
                if (errno == EACCES)
                {
                    log_printf("error: no permission\n");
                    response_status = "403 Forbidden";
                }
                else
                {
                    log_printf("error: errno = %d\n", errno);
                    response_status = "400 Bad Request";
                }
            }
            else
            {
                fclose(file);

                // create the mapping
                uint64_t uuid = (uint64_t)rand();
                uuid <<= 32;
                uuid |= (uint64_t)rand();
                mappings[uuid] = p;

                log_printf("created mapping: %llu - %s\n",
                           uuid, request->uri);
                response_status = "201 Created";
                sprintf(response_content, "%llu", uuid);
            }
        }
    }
    else
    {
        log_printf("error: not found\n");
        response_status = "404 Not Found";
    }
    
    assert(response_status != NULL);

    log_printf("responded with %s\n", response_status);
    mg_printf(conn, "HTTP/1.1 %s\r\n"
              "Content-Type: text/plain\r\n\r\n"
              "%s",
              response_status, response_content);
}

void handle_delete(mg_connection *conn,
                   const mg_request_info *request)
{
    const char *response_status = NULL;
    uint64_t uuid = atoll(request->uri + 1);
    log_printf("uuid requested: %llu\n", uuid);

    if (mappings.find(uuid) == mappings.end())
    {
        log_printf("uuid doesn't exist\n");
        response_status = "404 Not Found";
    }
    else
    {
        log_printf("mapping erased\n");
        mappings.erase(uuid);
        response_status = "200 OK";
    }

    log_printf("responded with %s\n", response_status);
    mg_printf(conn, "HTTP/1.1 %s\r\n"
              "Content-Type: text/plain\r\n\r\n"
              "%s", response_status);
}

void *callback(mg_event event,
               mg_connection *conn,
               const mg_request_info *request)
{
    if (event != MG_NEW_REQUEST)
        return (void*)1;


    log_printf("new request: =====================\n"
               "method: %s\n"
               "uri: %s\n"
               "ip: %lx\n",
               request->request_method,
               request->uri,
               request->remote_ip);


    if (!strcmp(request->request_method, "GET"))
    {
        if (request->remote_ip == 0x7f000001 && request->uri[1] == '\0')
        {
            log_printf("current state requested\n");
            
            mg_printf(conn, "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n\r\n");
            for (std::map<uint64_t, path>::iterator i = mappings.begin();
                 i != mappings.end(); ++i)
                mg_printf(conn, "\n%llu,%s", i->first, i->second.c_str());
        }
        else
            handle_get(conn, request);
    }
    else if (!strcmp(request->request_method, "POST"))
    {
        if (request->remote_ip != 0x7f000001)
            log_printf("invalid ip for POST request: %x\n", request->remote_ip);
        else
            handle_post(conn, request);
    }
    else if (!strcmp(request->request_method, "DELETE"))
    {
        if (request->remote_ip != 0x7f000001)
            log_printf("invalid ip for DELETE request: %x\n", request->remote_ip);
        else
            handle_delete(conn, request);
    }

    putchar('\n');

    return (void*)1;
}



// main
int main(int argc, char *argv[])
{
    srand(time(NULL));

    // parse the commandline arguments
    int opt = -1;
    while ((opt = getopt(argc, argv, "p:v")) != -1)
    {
        switch (opt)
        {
        case 'p':
            port = optarg;
            break;
        case 'v':
            verbose = true;
            break;
        }
    }
    // display the all the options
    log_printf("Starting server on port %s.\n", port.c_str());


    // start the server
    const char* options[] = {
        "listening_ports", port.c_str(),
        "enable_directory_listing", "no",
        NULL
    };
    mg_context *ctx = mg_start(callback, NULL, options);
    if (!ctx)
    {
        fputs("Failed to start server.\n", stderr);
        return 0;
    }
    log_printf("Server is now running. Press CTRL-C to quit.\n");

    // set the long jump
    if (setjmp(exit_env) != 0)
    {
        mg_stop(ctx);
        exit(EXIT_SUCCESS);
    }
    else
    {
        signal(SIGINT, sig_hand);
        signal(SIGTERM, sig_hand);
        signal(SIGQUIT, sig_hand);
    }

    // go to sleep for a very long time
    sleep(0xffffffff);
    
    return 0;
}
