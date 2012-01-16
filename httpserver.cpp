#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <stdint.h>
#include <assert.h>
#include <signal.h>
#include <string>
#include <map>
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


struct Resource
{
    path p;
    int count;
    time_t expiration_time;
};


// global variables
mt19937 rng(time(NULL));        // random number generator
mg_context *ctx = NULL;         // the server instance
std::string port = "1235";      // the port
bool use_upnp = false;          // whether we used UPnP to forward ports
UPNPUrls urls;                  // UPnP variables
IGDdatas data;
std::map<uint64_t, Resource> mappings; // maps from uuid to Resource
bool verbose = false;           // verbose flag


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


static void sig_hand(int code)
{
	log_printf("quitting...\n");
    mg_stop(ctx);
    if (use_upnp)
    {
        int r = UPNP_DeletePortMapping(
            urls.controlURL, data.first.servicetype,
			port.c_str(), "TCP", NULL);
		if (r)
			log_printf("failed to delete port mapping: %d\n", r);
		else
			log_printf("deleted port mapping.\n");
	}
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
    uint64_t uuid = isdigit(request->uri[1])? lexical_cast<uint64_t>(request->uri + 1) : 0;
    log_printf("uuid requested: %s\n", request->uri + 1);

    // make sure the uuid exists
    if (mappings.find(uuid) == mappings.end())
    {
        log_printf("uuid doesn't exist\n");
        response_status = "404 Not Found";
    }
    else
    {
        Resource &resource = mappings[uuid];
        path& p = resource.p;
        
        // check to see if the path is still valid
        response_status = check_path(p);
        if (response_status.length())
            mappings.erase(uuid);
        // make sure it hasn't expired
        else if (resource.expiration_time < time(NULL))
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

            // send the file
            mg_send_file(conn, to_utf8(p.c_str()), to_utf8(p.filename().c_str()));
            if (--resource.count == 0)
                mappings.erase(uuid);
            return;
        }
    }

    log_printf("responded with %s\n", response_status.c_str());
    mg_printf(conn, "HTTP/1.1 %s\r\n"
              "Content-Type: text/plain\r\n\r\n",
              response_status.c_str());
}


// handle POST requests
void handle_post(mg_connection *conn,
                 const mg_request_info *request)
{
    static uniform_int<uint64_t> uuid_gen(
        0x100000000, 0x4000000000000000);
    std::string response_status;
    std::string response_content;

    // do validity checking
#ifdef _WIN32
    path p(request->uri + 1);
#else
    path p(request->uri);
#endif
    response_status = check_path(p);
    if (response_status.length() == 0) // 0 means it's OK
    {
        // create the mapping
        uint64_t uuid = uuid_gen(rng);
        std::string uuid_str = lexical_cast<std::string>(uuid);

        // create resource for only 1 download, expires in 1 hour by default
        Resource resource;
        resource.p = p;
        resource.count = 1;
        int duration = 3600; // 3600 seconds

        // get the count and time query variables, if they are provided
        char body[128];
        char var[16];
        int body_size = mg_read(conn, body, sizeof(body));
		body[body_size] = '\0';
        log_printf("body: %d, %s\n", body_size, body);
        if (mg_get_var(body, body_size, "count", var, sizeof(var)) != -1)
            resource.count = atoi(var);
        if (mg_get_var(body, body_size, "time", var, sizeof(var)) != -1)
        {
            duration = atoi(var);
            resource.expiration_time = time(NULL) + duration;
        }

        // create the mapping
        mappings[uuid] = resource;

        log_printf("created mapping: %s - %s, count=%d, duration=%d\n",
                    uuid_str.c_str(), request->uri, resource.count, duration);
        response_status = "201 Created";
        response_content = lexical_cast<std::string>(uuid);
    }
    
    assert(response_status.length() > 0);

    log_printf("responded with %s\n", response_status.c_str());
    mg_printf(conn, "HTTP/1.1 %s\r\n"
              "Content-Type: text/plain\r\n\r\n%s",
              response_status.c_str(), response_content.c_str());
}


// handle DELETE requests
void handle_delete(mg_connection *conn,
                   const mg_request_info *request)
{
    // get the UUID
    const char *response_status = NULL;
    uint64_t uuid = isdigit(request->uri[1])? lexical_cast<uint64_t>(request->uri + 1) : 0;
    log_printf("uuid requested: %s\n", request->uri + 1);

    // find the UUID, and if found, delete it
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
    for (int i = 0; i < request->num_headers; ++i)
        log_printf("%s - %s\n", request->http_headers[i]);

    if (!strcmp(request->request_method, "GET"))
    {
        // if from localhost and no additional uri, then just list current mappings
        if (request->remote_ip == 0x7f000001 && request->uri[1] == '\0')
        {
            log_printf("current state requested\n");

            // remove expired mappings
            std::vector<uint64_t> expired;
            for (std::map<uint64_t, Resource>::iterator i = mappings.begin();
                 i != mappings.end(); ++i)
            {
                if (i->second.expiration_time < time(NULL))
                {
                    log_printf("file has expired: %s\n", i->second.p.c_str());
                    expired.push_back(i->first);
                }
            }
            for (size_t i = 0; i < expired.size(); ++i)
                mappings.erase(expired[i]);

            mg_printf(conn, "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n\r\n");
            for (std::map<uint64_t, Resource>::iterator i = mappings.begin();
                 i != mappings.end(); ++i)
                mg_printf(conn, "%s,%s,%s,%s\n",
                          lexical_cast<std::string>(i->first).c_str(),
                          i->second.count,
                          lexical_cast<std::string>(i->second.expiration_time).c_str(),
                          i->second.p.c_str());
        }
        else
            handle_get(conn, request);
    }
    else if (!strcmp(request->request_method, "POST"))
    {
        if (request->remote_ip != 0x7f000001) // only accept from localhost
            log_printf("invalid ip for POST request: %x\n", request->remote_ip);
        else
            handle_post(conn, request);
    }
    else if (!strcmp(request->request_method, "DELETE"))
    {
        if (request->remote_ip != 0x7f000001) // only accept from localhost
            log_printf("invalid ip for DELETE request: %x\n", request->remote_ip);
        else
            handle_delete(conn, request);
    }

    putchar('\n');

    return (void*)1;
}



// main
#ifdef _WIN32
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
    options_description desc("Allowed options");
    desc.add_options()
        ("help,h", "produce this help message")
        ("verbose,v", "turn on verbose mode")
        ("port,p", value<std::string>(), "specify the port to run on")
        ;
    variables_map vm;
    store(parse_command_line(argc, argv, desc), vm);
    notify(vm);

    if (vm.count("help"))
    {
        std::cout << desc << '\n';
        return 1;
    }

    if (vm.count("verbose"))
        verbose = true;
    if (vm.count("port"))
        port = vm["port"].as<std::string>();


#ifdef _WIN32
    WSADATA wsa_data;
    int wsa_result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
    if (wsa_result != NO_ERROR)
    {
        log_printf("WSAStartup() failed: %d\n", wsa_result);
        return 1;
    }
#endif

    // do UPnP discovery
    use_upnp = upnp_discovery();

    // start the server
    log_printf("Starting server on port %s...", port.c_str());
    const char* options[] =
    {
        "listening_ports", port.c_str(),
        "enable_directory_listing", "no",
        "num_threads", "1",
        NULL
    };
    ctx = mg_start(callback, NULL, options);
    if (!ctx)
    {
        log_printf("failed.\n");
        return 0;
    }
    log_printf("succeded.\n");

    signal(SIGINT, sig_hand);
    signal(SIGTERM, sig_hand);
#ifdef _WIN32
    signal(SIGBREAK, sig_hand);
#else
    signal(SIGQUIT, sig_hand);
#endif

    // write the port to a temporary settings file
    log_printf("writing settings file...");
    path settings_filename = temp_directory_path() / ".httpserver";
    FILE *settings_file = FOPEN(settings_filename.c_str(), T("w"));
    if (settings_file)
    {
        fputs(port.c_str(), settings_file);
        fclose(settings_file);
        log_printf("succeeded.\n");
    }
    else
        log_printf("failed.\n");

    // go to sleep for a very long time
    log_printf("Press CTRL-C to quit.\n");
    for (;;)
#ifdef _WIN32
        Sleep(0xffffffff);
#else
        sleep(0xffffffff);
#endif
    
    return 0;
}
