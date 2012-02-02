/* Copyright (c) 2012, Hui Peng Hu
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *    This product includes software developed by Hui Peng Hu.
 * 4. Neither the name of the <organization> nor the
 *    names of its contributors may be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY <COPYRIGHT HOLDER> ''AS IS'' AND ANY
 *    EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 *    WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 *    DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 *    DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 *    (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 *    LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 *    ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *    (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 *    SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <assert.h>
#include <signal.h>
#include <string>
#include <map>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/socket.h>
//#include <netinet/in.h>
#include <netdb.h>
#endif
#include <boost/filesystem.hpp>
#include <boost/random.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/program_options.hpp>
#include <miniupnpc/miniupnpc.h>
#include <miniupnpc/upnpcommands.h>
#include <archive.h>
#include <archive_entry.h>
#include "mongoose.h"
using namespace boost;
using namespace boost::filesystem;
using namespace boost::random;
using namespace boost::program_options;


// take care of some windows unicode stuff
#ifdef _WIN32
#define T(x) L##x
const char* to_utf8(const wchar_t* str)
{
    static char buffer[512]; // hopefully, 512 is long enough
    WideCharToMultiByte(CP_UTF8, 0, str, -1, buffer, sizeof(buffer), NULL, NULL);
    return buffer;
}
#define FOPEN _wfopen
#else
#define T(x) x
#define to_utf8(x) x
#define my_fopen(x) fopen
#define FOPEN fopen
#endif


// global variables
bool verbose = false;           // verbose flag
mt19937 rng(time(NULL));        // random number generator
uniform_smallint<uint16_t> port_gen(5000, 65535);

mg_context *ctx = NULL;         // the server instance
std::string port;               // the port
bool use_upnp = false;          // whether we used UPnP to forward ports
UPNPUrls urls;                  // UPnP variables
IGDdatas data;

path the_path;                  // path of the file
uint64_t the_uuid;              // uuid of the resource
int count;                      // how many downloads before expiration
time_t expiration_time;         // the time at which it expires

bool quit = false;


// functions for logging
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


// returns the HTTP status code string if error, else, NULL
std::string check_path(const path& p)
{
    boost::system::error_code ec;
    if (is_directory(p, ec))
    {
        // TODO: figure out how to validate directories
        return "";
    }
    else if (exists(p))
    {
        FILE *file = FOPEN(p.c_str(), T("r"));
        if (file)
        {
            fclose(file);
            return "";
        }
        else
        {
            log_printf("failed to open for reading. "
                "does it have permissions?\t%s\n", p.c_str());
            if (errno == EACCES)
                return "403 Forbidden";
            else
                return "400 Bad Request";
        }
    }
    else
        return "404 Not Found";
}


std::string get_external_ip()
{
    // do DNS lookup of whatismyip
    struct addrinfo hints;
    struct addrinfo *servinfo, *p;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    int status = getaddrinfo("automation.whatismyip.com", "http", &hints, &servinfo);
    if (status != 0) return "";

    // create the socket
    int sock;
    const char *query = "GET http://automation.whatismyip.com/n09230945.asp\r\n\r\n";
    for (p = servinfo; p != NULL; p = p->ai_next)
    {
        sock = socket(AF_INET, SOCK_STREAM, 0);
        if (socket < 0) continue;

        if (connect(sock, p->ai_addr, p->ai_addrlen) == -1)
        {
            close(sock);
            continue;
        }

        break;
    }
    if (p == NULL) return "";

    // send http request
    if (send(sock, query, strlen(query), 0) < 0)
    {
        close(sock);
        return "";
    }

    // get response
    char answer[256];
    int size = recv(sock, answer, sizeof(answer), 0);
    if (size < 0)
    {
        close(sock);
        return "";
    }
    close(sock);
    answer[size] = '\0';

    const char *pos = strstr(answer, "\r\n\r\n");
    if (pos == NULL) return "";
    return pos + 4;
}


// UPnP discovery
bool upnp_discovery()
{
    log_printf("Starting UPnP discovery.\n");
    int error = 0;

    // try to discover devices
    UPNPDev *devlist = upnpDiscover(2000, NULL, NULL, 0, 0, &error);
    if (!devlist)
    {
        log_printf("upnp discovery failed: %d\n", error);
        return false;
    }

    // try to find valid IGD device
    char lanaddr[64];	// my ip address on the LAN
    int i = UPNP_GetValidIGD(devlist, &urls, &data,
                             lanaddr, sizeof(lanaddr));
    if (i == 1)
        log_printf("Found valid IGD: %s\n", urls.controlURL);
    else
    {
        const char *more_info;
        if (i == 2)
            more_info = "Found a (not connected?) IGD:";
        else if (i == 3)
            more_info = "UPnP device found. Is it an IGD?";
        else
            more_info = "Found device:";
        log_printf("%s %s\nTrying to continue anyway\n",
                   more_info, urls.controlURL);
    }
    log_printf("Local LAN ip address: %s\n", lanaddr);

    // see if the external port is already mapped
    char intClient[40];
    char intPort[6];
    char duration[16];
    bool has_mapping = false;
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
        if (!strcmp(intClient, lanaddr)) // great if it's mapped to us
        {
            has_mapping = true;
            break;
        }

        log_printf("external port %s already taken.\n", port.c_str());
        port = lexical_cast<std::string>(port_gen(rng));
    }

    if (has_mapping)
    {
        log_printf("mapping already exists for port %s\n", port.c_str());
        return true;
    }

    // try to add the port mapping
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

// removes a UPnP mapping
void remove_upnp_mapping()
{
    int r = UPNP_DeletePortMapping(
        urls.controlURL, data.first.servicetype,
        port.c_str(), "TCP", NULL);
    if (r)
        log_printf("failed to delete port mapping: %d\n", r);
    else
        log_printf("deleted port mapping.\n");
}

static void sig_hand(int code)
{
    log_printf("quitting...\n");
    mg_stop(ctx);
    if (use_upnp)
        remove_upnp_mapping();
    exit(EXIT_SUCCESS);
}


// compress and entire directory
// assumes that directory_path is valid
void compress_directory(const path& directory_path,
                        const path& outname)
{
    log_printf("compressing directory \"%s\" into \"%s\"\n",
               directory_path.c_str(), outname.c_str());

    // declare and initialize variables
    struct archive *a = archive_write_new();
    archive_write_set_compression_gzip(a);
    archive_write_set_format_pax_restricted(a);
    archive_write_open_filename(a, to_utf8(outname.c_str()));
    struct archive_entry *entry = archive_entry_new();
    
    // offset of the parent directory's full path
	int len = (directory_path.parent_path().native().length() + 1);

    recursive_directory_iterator end;
    recursive_directory_iterator iter(directory_path);
    while (iter != end)
    {
        const path& p = iter->path();

        // ignore directories
        if (is_directory(p))
        {
            ++iter;
            continue;
        }
        
        // set headers
        archive_entry_set_pathname(entry, to_utf8(p.c_str()) + len); // add the offset to get rid of the absolute path
        archive_entry_set_size(entry, file_size(p));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        archive_write_header(a, entry);

        // open the file for reading
        FILE *file = FOPEN(p.c_str(), T("rb"));
        if (!file)
        {
            log_printf("failed to open file for compression: %s\n", p.c_str());
            continue;
        }

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
    std::string response_status;
    uint64_t uuid = 0;
    try
    {
        uuid = isdigit(request->uri[1])? lexical_cast<uint64_t>(request->uri + 1) : 0;
    }
    catch (bad_lexical_cast ex)
    { }
    log_printf("uuid requested: %s\n", request->uri + 1);

    // make sure the uuid exists
    if (uuid != the_uuid)
    {
        log_printf("uuid not correct\n");
        response_status = "404 Not Found";
    }
    else
    {
        path& p = the_path;
        
        // check to see if the path is still valid
        response_status = check_path(p);
        if (response_status.length())
            quit = true;
        else
        {
            // if it's a directory, compress it
            if (is_directory(p))
            {
                path::string_type filename = p.filename().native();
                if (filename[0] == '.')
                    filename.erase(0, 1);
                path new_path = temp_directory_path() / filename;

                new_path.replace_extension(".tgz");
                compress_directory(p, new_path);
                p = new_path;
            }

            // send the file
            mg_send_file(conn, to_utf8(p.c_str()), to_utf8(p.filename().c_str()));
            log_printf("finished sending the file.\n");
            if (--count == 0)
                quit = true;
            return;
        }
    }

    log_printf("responded with %s\n", response_status.c_str());
    mg_printf(conn, "HTTP/1.1 %s\r\n"
              "Content-Type: text/plain\r\n\r\n",
              response_status.c_str());
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
    for (int i = 0; i < request->num_headers; ++i)
        log_printf("%s - %s\n", request->http_headers[i]);

    if (!strcmp(request->request_method, "GET"))
        handle_get(conn, request);

    putchar('\n');

//    if (quit)
//        raise(SIGTERM);

    return (void*)1;
}



// main
#if defined(_WIN32) && defined(NDEBUG)
int APIENTRY WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, 
                     LPSTR lpCmdLine, int nCmdShow)
{
    int argc = __argc;
    char **argv = __argv;
#else
int main(int argc, char *argv[])
{
#endif
    // parse the commandline arguments
    options_description desc("Usage: easytransfer [options] path\nAllowed options");
    desc.add_options()
        ("path", value<std::string>(), "path of the file/folder (required, can also the last argument)")
        ("count,c", value<int>()->default_value(2), "maximum download count before the link expires")
        ("duration,d", value<unsigned int>()->default_value(30), "time before the link expires, in minutes")
        ("verbose,v", "turn on verbose mode")
        ("help,h", "produce this help message")
        ;
    positional_options_description pos_desc;
    pos_desc.add("path", 1);
    parsed_options parsed = command_line_parser(argc, argv).options(desc).positional(pos_desc).run();
    variables_map vm;
    store(parsed, vm);
    notify(vm);

    if (!vm.count("path"))
    {
        std::cout << desc << '\n';
        return EXIT_FAILURE;
    }
    else
        the_path = vm["path"].as<std::string>();
    count = vm["count"].as<int>();
    unsigned int duration = vm["duration"].as<unsigned int>() * 60; // * 60 to get seconds
    expiration_time = time(NULL) * 60 + duration;
    if (vm.count("help"))
    {
        std::cout << desc << '\n';
        return 0;
    }
    verbose = vm.count("verbose") > 0;
    

    // check the path first
    log_printf("checking path: %s\n", the_path.c_str());
    std::string path_status = check_path(the_path);
    if (path_status.length() > 0)
    {
        std::cout << path_status << '\n';
        return EXIT_FAILURE;
    }


    // call WSAStartup on windows
#ifdef _WIN32
    WSADATA wsa_data;
    int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != NO_ERROR)
    {
        log_printf("WSAStartup() failed: %d\n", wsa_result);
        return EXIT_FAILURE;
    }
#endif

    // get external ip, and quit if that failed
    std::string external_ip = get_external_ip();
    if (external_ip.length() == 0)
    {
        log_printf("failed to get external ip\n");
        return EXIT_FAILURE;
    }

    // try to start server
    int i;
    for (i = 0; i < 10; ++i)
    {
        // generate new random port
        port = lexical_cast<std::string>(port_gen(rng));

        // try to do UPnP discovery
        use_upnp = upnp_discovery();

        // start the server
        log_printf("Starting server on port %s...", port.c_str());
        const char* options[] =
        {
            "listening_ports", port.c_str(),
            "enable_directory_listing", "no",
            NULL
        };
        ctx = mg_start(callback, NULL, options);
        if (ctx)
            break;
        else
            remove_upnp_mapping();
    }
    if (i == 10)
    {
        log_printf("failed to start server after trying many times.\n");
        return EXIT_FAILURE;
    }
    else
        log_printf("succeded.\n");

    // setup the signal handlers
    signal(SIGINT, sig_hand);
    signal(SIGTERM, sig_hand);
#ifdef _WIN32
    signal(SIGBREAK, sig_hand);
#else
    signal(SIGQUIT, sig_hand);
#endif

    // create the UUID, and hence, the full link, and print it
    the_uuid = uniform_int<uint64_t>(0x100000000, 0x4000000000000000)(rng);
    std::cout << "http://" << external_ip << ':' << port << '/' << the_uuid << '\n';

    // go to sleep for a very long time (want a better way for this)
    log_printf("Press CTRL-C to quit.\n");
#ifdef _WIN32
    Sleep(duration * 1000);
#else
    sleep(duration);
#endif

    raise(SIGTERM);
    
    return 0;
}
