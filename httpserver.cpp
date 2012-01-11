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
#include <boost/random.hpp>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <archive.h>
#include <archive_entry.h>
#include "mongoose.h"
using namespace boost;
using namespace boost::filesystem;
using namespace boost::random;


struct Resource
{
    path p;
    int count;
    time_t expiration_time;
};


mt19937 rng(time(NULL));
std::string port = "1235";
std::map<uint64_t, Resource> mappings;


// utility functions
const char* to_string(int i)
{
    static char buffer[11];
    sprintf(buffer, "%d\n", i);
    return buffer;
}


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


// UPnP discovery
UPNPUrls urls;
IGDdatas data;
bool upnp_discovery()
{
    log_printf("Starting UPnP discovery.\n");
    int error = 0;

    UPNPDev *devlist = upnpDiscover(2000, NULL, NULL, 0, 0, &error);
    if (!devlist)
    {
        log_printf("upnp discovery failed.\n");
        return false;
    }

    char lanaddr[64];	/* my ip address on the LAN */

    int i = UPNP_GetValidIGD(devlist, &urls, &data,
                             lanaddr, sizeof(lanaddr));
    switch (i)
    {
    case 1:
        log_printf("Found valid IGD: %s\n", urls.controlURL);
        break;
    case 2:
        log_printf("Found a (not connected?) IGD: %s\n"
                   "Trying to continue anyway\n",
                   urls.controlURL);
        break;
    case 3:
        log_printf("UPnP device found. Is it an IGD? %s\n"
                   "Trying to continue anyway\n",
                   urls.controlURL);
        break;
    default:
        log_printf("Found device: %s\n",
                   "Trying to continue anyway\n",
                   urls.controlURL);
    }
    log_printf("Local LAN ip address: %s\n", lanaddr);


    char intClient[40];
    char intPort[6];
    char duration[16];
    bool has_mapping = false;
    static uniform_smallint<uint16_t> port_gen(5000, 65535);
    for (;;)
    {
        int r = UPNP_GetSpecificPortMappingEntry(
            urls.controlURL,
            data.first.servicetype,
            port.c_str(),
            "TCP",
            intClient, intPort, NULL, NULL, duration);

        if (r != UPNPCOMMAND_SUCCESS)
            break;
        if (!strcmp(intClient, lanaddr))
        {
            has_mapping = true;
            break;
        }

        log_printf("external port %s already taken.\n", port.c_str());
        port = to_string(port_gen(rng));
    }

    if (has_mapping)
    {
        log_printf("mapping already exists for port %s\n", port.c_str());
        return true;
    }

    int r = UPNP_AddPortMapping(
        urls.controlURL,
        data.first.servicetype,
        port.c_str(),
        port.c_str(),
        lanaddr,
        NULL,
        "TCP",
        NULL,
        "0");
    if (r)
    {
        log_printf("mapping failed for port %s\n", port.c_str());
        return false;
    }
    
    log_printf("mapping added for port %s\n", port.c_str());
    return true;
}


// compress and entire directory
// assumes that directory_path is valid
void compress_directory(const path& directory_path,
                        const path& outname)
{
    struct archive *a = archive_write_new();
    archive_write_set_compression_gzip(a);
    archive_write_set_format_pax_restricted(a);
    archive_write_open_filename(a, outname.c_str());
    struct archive_entry *entry = archive_entry_new();
    
    int len = strlen(directory_path.parent_path().c_str()) + 1;

    recursive_directory_iterator end;
    recursive_directory_iterator iter(directory_path);
    while (iter != end)
    {
        const path& p = iter->path();
        iter++;

        if (is_directory(p))
        {
            ++iter;
            continue;
        }

        archive_entry_set_pathname(entry, p.c_str() + len);
        archive_entry_set_size(entry, file_size(p));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_write_header(a, entry);

        FILE *file = fopen(p.c_str(), "rb");
        char buffer[8192];
        int len;
        do
        {
            len = fread(buffer, 1, sizeof(buffer), file);
            archive_write_data(a, buffer, len);
        } while (len == sizeof(buffer));
        fclose(file);
        archive_entry_clear(entry);
        
        ++iter;
    }

    archive_write_close(a);
    archive_write_finish(a);
}


// handle GET requests
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
        Resource &resource = mappings[uuid];
        path& p = resource.p;

        if (!exists(p))
        {
            log_printf("file no longer exists: %s\n", p.c_str());
            mappings.erase(uuid);
            response_status = "410 Gone";
        }
        if (resource.expiration_time < time(NULL))
        {
            log_printf("file has expired: %s\n", p.c_str());
            mappings.erase(uuid);
            response_status = "410 Gone";
        }
        else
        {
            // if it's a directory, compress it
            if (is_directory(p))
            {
                path new_path = temp_directory_path() / p.filename();
                new_path.replace_extension(".tgz");
                compress_directory(p, new_path);
                p = new_path;
            }

            FILE *file = fopen(p.c_str(), "r");
            if (file == NULL)
            {
                log_printf("failed to open file: %s\n", p.c_str());
                mappings.erase(uuid);
                response_status = "410 Gone";
            }
            else
            {
                fclose(file);

                mg_send_file(conn, p.c_str(), p.filename().c_str());
                if (--resource.count == 0)
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


// handle POST requests
void handle_post(mg_connection *conn,
                 const mg_request_info *request)
{
    static uniform_int<uint64_t> uuid_gen(
        0x100000000LL, 0x4000000000000000LL);
    const char *response_status = NULL;
    char response_content[256] = "";

    path p(request->uri);
    if (exists(p))
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
            uint64_t uuid = uuid_gen(rng);

            Resource resource;
            resource.p = p;
            resource.count = 1;
            int duration = 3600;

            char body[128];
            char var[16];
            int body_size = mg_read(conn, body, sizeof(body));
            log_printf("body: %d, %s\n", body_size, body);
            if (mg_get_var(body, body_size, "count", var, sizeof(var)) != -1)
                resource.count = atoi(var);
            if (mg_get_var(body, body_size, "time", var, sizeof(var)) != -1)
            {
                duration = atoi(var);
                resource.expiration_time = time(NULL) + duration;
            }

            mappings[uuid] = resource;

            log_printf("created mapping: %llu - %s, count = %d, duration = %d\n",
                       uuid, request->uri, resource.count, duration);
            response_status = "201 Created";
            sprintf(response_content, "%llu", uuid);
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


// handle DELETE requests
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


// HTTP callback
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
            for (std::map<uint64_t, Resource>::iterator i = mappings.begin();
                 i != mappings.end(); ++i)
                mg_printf(conn, "\n%llu,%s", i->first, i->second.p.c_str());
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


    // do UPnP discovery
    bool use_upnp = upnp_discovery();

    // start the server
    log_printf("Starting server on port %s...", port.c_str());
    const char* options[] =
    {
        "listening_ports", port.c_str(),
        "enable_directory_listing", "no",
        NULL
    };
    mg_context *ctx = mg_start(callback, NULL, options);
    if (!ctx)
    {
        fputs("failed.\n", stderr);
        return 0;
    }
    log_printf("succeded.\nPress CTRL-C to quit.\n");

    // set the long jump
    if (setjmp(exit_env) != 0)
    {
        mg_stop(ctx);
        if (use_upnp)
        {
            int r = UPNP_DeletePortMapping(
                urls.controlURL, data.first.servicetype,
                port.c_str(), "TCP", NULL);
            if (r)
                log_printf("failed to delete port mapping: %d\n", r);
        }
        exit(EXIT_SUCCESS);
    }
    else
    {
        signal(SIGINT, sig_hand);
        signal(SIGTERM, sig_hand);
        signal(SIGQUIT, sig_hand);
    }

    log_printf("writing settings file...");
    path settings_filename = temp_directory_path() / ".httpserver";
    FILE *settings_file = fopen(settings_filename.c_str(), "w");
    if (settings_file)
    {
        fputs(port.c_str(), settings_file);
        fclose(settings_file);
        log_printf("succeeded.\n");
    }
    else
        log_printf("failed.\n");

    // go to sleep for a very long time
    for (;;)
        sleep(0xffffffff);
    
    return 0;
}
