#include <ptmp/api.h>
#include "CLI11.hpp"
#include <czmq.h>


#include <vector>
#include <string>
#include <unordered_map>
#include <cstdio>

struct file_opt_t {
    FILE* fp;
    std::string filename;
};

void make_file_opts(CLI::App* app, file_opt_t& fopt)
{
    app->add_option("-f,--filename", fopt.filename, "Name of file.");
}

struct sock_opt_t {
    zsock_t* sock;
    int hwm;
    std::string socktype;
    std::string attach;
    std::vector<std::string> endpoints;
    std::vector<std::string> subscriptions; 
};
void make_sock_opts(CLI::App* app, sock_opt_t& sopt)
{
    app->add_option("-m,--socket-hwm", sopt.hwm,
                   "The ZeroMQ socket highwater mark");    
    app->add_option("-p,--socket-pattern", sopt.socktype,
                    "The ZeroMQ socket pattern for endpoint [PUB, PAIR, PUSH]");
    app->add_option("-a,--socket-attachment", sopt.attach,
                    "The socket attachement method [bind|connect] for this endpoint");
    app->add_option("-s,--subscriptions", sopt.subscriptions,
                    "Any subscriptions if SUB.");
    app->add_option("-e,--socket-endpoints", sopt.endpoints,
                    "The socket endpoint addresses in Zero MQ format (tcp:// or ipc://)")->expected(-1);
}

int make_sock(sock_opt_t& sopt)
{
    if (sopt.endpoints.empty()) {
        return 0;               // allowed
    }
    const std::unordered_map<std::string, int> socktypemap{
        {"PUB",ZMQ_PUB},{"SUB",ZMQ_SUB},{"PUSH",ZMQ_PUSH},{"PULL",ZMQ_PULL}
    };
    const auto& st = socktypemap.find(sopt.socktype);
    if (st == socktypemap.end()) {
        zsys_error("Unknown socket type: \"%s\"", sopt.socktype.c_str());
        return -1;
    }
    int socktype = st->second;
    sopt.sock = zsock_new(socktype);
    if (socktype == ZMQ_SUB) {
        if (sopt.subscriptions.empty()) {
            zsock_set_subscribe(sopt.sock, "");
            //zsys_debug("subscribing to default");
        }
        else {
            for (auto sub : sopt.subscriptions) {
                zsock_set_subscribe(sopt.sock, sub.c_str());
                //zsys_debug("subscribing to \"%s\"", sub.c_str());
            }
        }
    }
    if (sopt.attach == "connect") {
        for (auto ep : sopt.endpoints) {
            zsock_connect(sopt.sock, ep.c_str(), NULL);
        }
    }
    if (sopt.attach == "bind") {
        for (auto ep : sopt.endpoints) {
            zsock_bind(sopt.sock, ep.c_str(), NULL);
        }
    }
    return 0;
}


// Every message data is assumed to be prefixed by a 64 bit unsigned
// int holding the size of the message data in bytes.
zmsg_t* read_msg(FILE* fp)
{
    size_t size=0;
    int nread = fread(&size, sizeof(size_t), 1, fp);
    if (nread != 1) {
        return NULL;
    }
    zchunk_t* chunk = zchunk_read(fp, size);
    zframe_t* frame = zchunk_pack(chunk);
    zchunk_destroy(&chunk);
    zmsg_t* msg = zmsg_decode(frame);
    zframe_destroy(&frame);
    return msg;
}

int write_msg(FILE* fp, zmsg_t* msg)
{
    zframe_t* frame = zmsg_encode(msg);
    zchunk_t* chunk = zchunk_unpack(frame);
    zframe_destroy(&frame);
    size_t size = zchunk_size(chunk);
    fwrite(&size, sizeof(size_t), 1, fp);
    int rc = zchunk_write(chunk, fp);
    zchunk_destroy(&chunk);
    return rc;
}


static
zmsg_t* fix_msg(zmsg_t*& msg)
{
    ptmp::data::TPSet tps;
    ptmp::internals::recv(msg, tps); // throws
    
    for (auto& tp : *tps.mutable_tps()) {
        tp.set_tspan(tp.tspan() * 25);
    }
    //zsys_debug("fix %ld TPs", tps.tps().size());

    // Fixme: I've ripped this out of ptmp/internals.cc.  It really
    // should be packaged somewhere.
    const int topic = 0;
    zmsg_t* newmsg = zmsg_new();
    zframe_t* fid = zframe_new(&topic, sizeof(int));
    int rc = zmsg_append(newmsg, &fid);
    size_t siz = tps.ByteSize();
    zframe_t* pay = zframe_new(NULL, siz);
    tps.SerializeToArray(zframe_data(pay), zframe_size(pay));
    rc = zmsg_append(newmsg, &pay);
    zmsg_destroy(&msg);
    return newmsg;
}


int main(int argc, char* argv[])
{
    zsys_init();

    CLI::App app{"fix times"};

    int number=0;
    app.add_option("-n,--number", number,
                   "Send at most this number of messages before terminating");

    int loop_delay=0;
    app.add_option("-l,--loop-delay-ms", loop_delay, "Loop delay in msec");

    int debug=0;
    app.add_option("-d,--debug", debug,
                   "Debug level. 0=errors, 1=summary, 2=verbose");

    int begwait=0;
    app.add_option("-B,--begin-wait-ms", begwait,
                   "Number of ms to wait between creating socket and starting to send");
    int endwait=0;
    app.add_option("-E,--end-wait-ms", endwait,
                   "Number of ms to wait between completing --count messages and terminating");

    // Sockets
    CLI::App* isock = app.add_subcommand("isock", "Input socket specification");
    CLI::App* osock = app.add_subcommand("osock", "Output socket specification");
    sock_opt_t isopt{NULL,1000,"SUB", "connect"}, osopt{NULL,1000,"PUB", "bind"};
    make_sock_opts(isock, isopt);
    make_sock_opts(osock, osopt);
    
    // files
    CLI::App* ifile = app.add_subcommand("ifile", "Input file specification");
    CLI::App* ofile = app.add_subcommand("ofile", "Output file specification");
    file_opt_t ifopt{NULL,""}, ofopt{NULL,""};
    make_file_opts(ifile, ifopt);
    make_file_opts(ofile, ofopt);

    CLI11_PARSE(app, argc, argv);
    
    if (make_sock(isopt) < 0) {
        return -1;
    }
    if (make_sock(osopt) < 0) {
        return -1;
    }

    if (!ifopt.filename.empty()) {
        ifopt.fp = fopen(ifopt.filename.c_str(), "r");
    }
    if (!ofopt.filename.empty()) {
        ofopt.fp = fopen(ofopt.filename.c_str(), "w");
    }

    if (!(ifopt.fp or isopt.sock)) {
        zsys_error("Neither file nor socket input be");
        return -1;
    }

    zclock_sleep (begwait);

    int count=0;
    while (true) {

        if (number and count >= number) {
            break;
        }
        ++count;

        zmsg_t* msg=NULL;
        if (ifopt.fp) {
            msg = read_msg(ifopt.fp);
            if (!msg) {
                if (debug>=1) {
                    zsys_info("End of file reached");
                }
                break;
            }
        }
        if (isopt.sock) {
            msg = zmsg_recv(isopt.sock);
            if (!msg) {
                zsys_warning("interupted");
                break;
            }
            if (debug>=1) {
                zsys_debug("got message");
            }
        }

        if (loop_delay) {
            zclock_sleep(loop_delay);
        }

        msg = fix_msg(msg);

        if (ofopt.fp) {
            write_msg(ofopt.fp, msg);
        }
        if (osopt.sock) {
            if (debug>=2) { zsys_debug("send message"); }
            int rc = zmsg_send(&msg, osopt.sock);
            if (rc != 0) {
                break;
            }
        }

        if (msg) {
            zmsg_destroy(&msg);
        }
    }

    if (debug>=1) {
        zsys_info("sleeping at end");
    }
    zclock_sleep(endwait);

    if (ofopt.fp) {
        fclose(ofopt.fp);
    }
    if (isopt.sock) {
        zsock_destroy(&isopt.sock);
    }
    if (osopt.sock) {
        zsock_destroy(&osopt.sock);
    }
    
    return 0;
}
