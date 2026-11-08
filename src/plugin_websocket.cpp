// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/socket_compat.hpp"
#include "velin/plugin_websocket.hpp"
#include "velin/system.hpp"
#include "velin/core.hpp"
#include <atomic>
#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <stdexcept>

namespace velin {
namespace {
void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }
bool validUtf8(std::string_view value){return value.empty() || !fromUtf8(value).empty();}
std::string lower(std::string text) { for (auto& c:text) c=static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return text; }
std::string trim(std::string text) { const auto first=text.find_first_not_of(" \t\r\n"); return first==std::string::npos?"":text.substr(first,text.find_last_not_of(" \t\r\n")-first+1); }
std::string acceptKey(const std::string& key) {
    const auto input=key+"258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto digest=sys::sha1(input);
    return sys::base64Encode(digest.data(),digest.size());
}
std::string frame(unsigned opcode,std::string_view payload) {
    std::string result(1,static_cast<char>(0x80|opcode)); const auto length=payload.size();
    if (length<126) result+=static_cast<char>(length);
    else if (length<=65535) { result+=static_cast<char>(126); result+=static_cast<char>(length>>8); result+=static_cast<char>(length); }
    else { result+=static_cast<char>(127); for (int shift=56;shift>=0;shift-=8) result+=static_cast<char>(static_cast<std::uint64_t>(length)>>shift); }
    result.append(payload); return result;
}
}
struct PluginWebSocketServer::Impl {
    struct Client {
        SOCKET socket=INVALID_SOCKET; std::string id,input,output,fragment; unsigned fragmentType=0;
        bool ready=false,closing=false;
        std::chrono::steady_clock::time_point created=std::chrono::steady_clock::now();
        std::deque<std::pair<size_t,std::string>> completions;
    };
    struct Send { std::string connection,text,token; };
    Json options; SOCKET listener=INVALID_SOCKET; std::atomic<std::uint16_t> boundPort{0};
    std::jthread worker; std::mutex mutex; std::deque<SocketEvent> events; std::deque<Send> sends;
    size_t maximum=1024*1024,connections=32; bool winsock=false; std::uint64_t serial=0;
    void event(std::string name,Json args) { std::lock_guard lock(mutex); if(events.size()<4096) events.push_back({std::move(name),std::move(args)}); }
    void stop() { if(worker.joinable()){worker.request_stop();worker.join();} if(listener!=INVALID_SOCKET){closesocket(listener);listener=INVALID_SOCKET;} boundPort=0; }
    bool handshake(Client& client) {
        const auto end=client.input.find("\r\n\r\n");
        if(end==std::string::npos){require(client.input.size()<=16384,"Handshake header too large");return false;}
        require(end<=16384,"Handshake header too large");
        std::istringstream input(client.input.substr(0,end)); std::string line,method,path,version;
        std::getline(input,line); std::istringstream request(line); request>>method>>path>>version;
        require(method=="GET" && version=="HTTP/1.1","Invalid upgrade request");
        require(options.value("path",path)==path,"Incorrect WebSocket path");
        std::map<std::string,std::string> headers;
        while(std::getline(input,line)){const auto split=line.find(':');if(split==std::string::npos)continue;auto name=lower(trim(line.substr(0,split)));auto text=trim(line.substr(split+1));if(headers.contains(name))headers[name]+=","+text;else headers[name]=text;}
        require(lower(headers["upgrade"])=="websocket" && headers["sec-websocket-version"]=="13","Unsupported upgrade");
        bool upgrade=false;std::istringstream tokens(lower(headers["connection"]));while(std::getline(tokens,line,','))if(trim(line)=="upgrade")upgrade=true;
        require(upgrade,"Missing upgrade connection token");
        std::string key;
        require(headers["sec-websocket-key"].size()==24 && sys::base64Decode(headers["sec-websocket-key"],key) && key.size()==16,"Invalid WebSocket key");
        if(options.contains("allowedOrigins")){const auto origins=options["allowedOrigins"].get<std::vector<std::string>>();require(std::find(origins.begin(),origins.end(),headers["origin"])!=origins.end(),"Origin not permitted");}
        std::string protocol;
        if(options.contains("protocols")){const auto protocols=options["protocols"].get<std::vector<std::string>>();std::istringstream offered(headers["sec-websocket-protocol"]);while(std::getline(offered,line,','))if(std::find(protocols.begin(),protocols.end(),trim(line))!=protocols.end()){protocol=trim(line);break;}}
        client.output="HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Accept: "+acceptKey(headers["sec-websocket-key"])+"\r\n";
        if(!protocol.empty())client.output+="Sec-WebSocket-Protocol: "+protocol+"\r\n";
        client.output+="\r\n";client.input.erase(0,end+4);client.ready=true;
        event("newConnection",Json::array({client.id,Json{{"path",path},{"protocol",protocol}}}));event("connectionState",Json::array({client.id,"ready"}));return true;
    }
    void parse(Client& client) {
        while(client.input.size()>=2 && !client.closing){
            const auto* bytes=reinterpret_cast<const unsigned char*>(client.input.data());const bool final=(bytes[0]&128)!=0;const unsigned opcode=bytes[0]&15;
            require((bytes[0]&112)==0 && (bytes[1]&128)!=0,"Invalid frame flags");
            size_t offset=2;std::uint64_t length=bytes[1]&127;
            if(length==126){if(client.input.size()<4)return;length=(bytes[2]<<8)|bytes[3];offset=4;require(length>=126,"Noncanonical frame length");}
            else if(length==127){if(client.input.size()<10)return;require((bytes[2]&128)==0,"Invalid frame length");length=0;for(int i=2;i<10;++i)length=(length<<8)|bytes[i];offset=10;require(length>65535,"Noncanonical frame length");}
            require(length<=maximum,"Frame exceeds message limit");require(opcode<8 || (final && length<=125),"Invalid control frame");
            if(client.input.size()<offset+4+length)return;
            std::string payload(static_cast<size_t>(length),'\0');for(size_t i=0;i<payload.size();++i)payload[i]=static_cast<char>(bytes[offset+4+i]^bytes[offset+i%4]);
            client.input.erase(0,offset+4+static_cast<size_t>(length));
            if(opcode==8){require(payload.size()!=1,"Invalid close payload");client.output+=frame(8,payload);client.closing=true;return;}
            if(opcode==9){client.output+=frame(10,payload);continue;}if(opcode==10)continue;
            require(opcode<=2,"Unknown opcode");
            if(opcode==0)require(client.fragmentType!=0,"Unexpected continuation");
            else {require(client.fragmentType==0,"Interrupted fragmented message");client.fragmentType=opcode;}
            require(client.fragment.size()+payload.size()<=maximum,"Message exceeds limit");client.fragment+=payload;
            if(final){const bool text=client.fragmentType==1;require(!text || validUtf8(client.fragment),"Invalid UTF8 message");
                Json data{{"bytes",std::vector<std::uint8_t>(client.fragment.begin(),client.fragment.end())},{"text",text?Json(client.fragment):Json{}}};
                event("message",Json::array({client.id,data}));client.fragment.clear();client.fragmentType=0;}
        }
    }
    void run(std::stop_token stop) {
        std::map<SOCKET,Client> clients;event("state",Json::array({"ready"}));
        while(!stop.stop_requested()){
            std::deque<Send> pending;{std::lock_guard lock(mutex);pending.swap(sends);}
            for(auto& send:pending){auto found=std::find_if(clients.begin(),clients.end(),[&](const auto& pair){return pair.second.id==send.connection && pair.second.ready && !pair.second.closing;});
                if(found==clients.end())event("sent",Json::array({send.token,true,"no_connection"}));
                else if(found->second.output.size()+send.text.size()>maximum*4)event("sent",Json::array({send.token,false,"Send queue full"}));
                else {auto& client=found->second;client.output+=frame(1,send.text);client.completions.push_back({client.output.size(),std::move(send.token)});}}
            fd_set reads,writes;FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(listener,&reads);
            SOCKET highest=listener;
            for(const auto& [socket,client]:clients){FD_SET(socket,&reads);if(!client.output.empty())FD_SET(socket,&writes);highest=std::max(highest,socket);}
            timeval timeout{0,50000};if(select(net::selectWidth(highest),&reads,&writes,nullptr,&timeout)==SOCKET_ERROR){event("state",Json::array({"failed","Socket select failed"}));break;}
            if(FD_ISSET(listener,&reads)){
                SOCKET socket=accept(listener,nullptr,nullptr);
                if(socket!=INVALID_SOCKET){if(clients.size()>=connections)closesocket(socket);else{net::setNonBlocking(socket);net::suppressSigpipe(socket);Client client;client.socket=socket;client.id=std::to_string(++serial);clients.emplace(socket,std::move(client));}}
            }
            for(auto iterator=clients.begin();iterator!=clients.end();){auto& client=iterator->second;bool remove=false;
                try{
                    if(FD_ISSET(client.socket,&reads)){char buffer[8192];const int count=static_cast<int>(recv(client.socket,buffer,sizeof(buffer),0));if(count==0)remove=true;else if(count<0){if(!net::wouldBlock(net::lastError()))remove=true;}else{client.input.append(buffer,static_cast<size_t>(count));require(client.input.size()<=maximum+16384,"Receive buffer exceeded");if(client.ready || handshake(client))parse(client);}}
                    if(FD_ISSET(client.socket,&writes) && !client.output.empty()){const int count=static_cast<int>(send(client.socket,client.output.data(),static_cast<int>(std::min<size_t>(client.output.size(),65536)),0));if(count>0){client.output.erase(0,static_cast<size_t>(count));for(auto& completion:client.completions)completion.first-=std::min(completion.first,static_cast<size_t>(count));while(!client.completions.empty() && client.completions.front().first==0){event("sent",Json::array({client.completions.front().second,true,"success"}));client.completions.pop_front();}}else if(count<0 && !net::wouldBlock(net::lastError()))remove=true;}
                    if(client.closing && client.output.empty())remove=true;
                    if(!client.ready && std::chrono::steady_clock::now()-client.created>std::chrono::seconds(10))remove=true;
                }catch(const std::exception& error){event("connectionState",Json::array({client.id,"failed",error.what()}));remove=true;}
                if(remove){for(const auto& completion:client.completions)event("sent",Json::array({completion.second,false,"Connection closed"}));event("connectionState",Json::array({client.id,"cancelled"}));closesocket(client.socket);iterator=clients.erase(iterator);}else ++iterator;
            }
        }
        for(const auto& [socket,client]:clients){event("connectionState",Json::array({client.id,"cancelled"}));closesocket(socket);}event("state",Json::array({"cancelled"}));
    }
};
PluginWebSocketServer::PluginWebSocketServer():impl_(std::make_unique<Impl>()){require(net::startup(),"Cannot initialize Winsock");impl_->winsock=true;}
PluginWebSocketServer::~PluginWebSocketServer(){impl_->stop();if(impl_->winsock)net::cleanup();}
void PluginWebSocketServer::create(const Json& options){impl_->stop();require(options.contains("port") && options["port"].is_number_integer(),"WebSocket port is required");const int port=options["port"].get<int>();require(port>=0 && port<=65535,"Invalid WebSocket port");impl_->options=options;impl_->maximum=std::clamp(options.value("maxMessageBytes",size_t(1048576)),size_t(1),size_t(16*1048576));impl_->connections=std::clamp(options.value("maximumConnections",size_t(32)),size_t(1),size_t(FD_SETSIZE-1));impl_->event("state",Json::array({"setup"}));}
void PluginWebSocketServer::start(){require(!impl_->options.is_null() && !impl_->worker.joinable(),"WebSocket server has not been created or is already started");
    const auto host=impl_->options.value("host",std::string("127.0.0.1"));const int family=host.find(':')!=std::string::npos?AF_INET6:AF_INET;
    SOCKET listener=socket(family,SOCK_STREAM,IPPROTO_TCP);require(listener!=INVALID_SOCKET,"Cannot create listener");
    try{net::setExclusiveAddress(listener);sockaddr_storage address{};net::socklen size=0;
        if(family==AF_INET){auto* value=reinterpret_cast<sockaddr_in*>(&address);value->sin_family=AF_INET;value->sin_port=htons(impl_->options["port"].get<unsigned short>());require(inet_pton(AF_INET,host.c_str(),&value->sin_addr)==1,"Host must be a numeric address");size=sizeof(*value);}
        else{auto* value=reinterpret_cast<sockaddr_in6*>(&address);value->sin6_family=AF_INET6;value->sin6_port=htons(impl_->options["port"].get<unsigned short>());require(inet_pton(AF_INET6,host.c_str(),&value->sin6_addr)==1,"Host must be a numeric address");size=sizeof(*value);}
        require(bind(listener,reinterpret_cast<sockaddr*>(&address),size)==0 && listen(listener,SOMAXCONN)==0,"Cannot bind WebSocket listener");require(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)==0,"Cannot read listener port");impl_->boundPort=ntohs(family==AF_INET?reinterpret_cast<sockaddr_in*>(&address)->sin_port:reinterpret_cast<sockaddr_in6*>(&address)->sin6_port);net::setNonBlocking(listener);impl_->listener=listener;impl_->worker=std::jthread([this](std::stop_token stop){impl_->run(stop);});
    }catch(...){closesocket(listener);throw;}}
void PluginWebSocketServer::stop(){impl_->stop();}
void PluginWebSocketServer::sendText(std::string connection,std::string text,std::string token){require(impl_->worker.joinable(),"WebSocket server is not running");require(text.size()<=impl_->maximum && validUtf8(text),"Invalid text message");std::lock_guard lock(impl_->mutex);require(impl_->sends.size()<1024,"Send queue full");impl_->sends.push_back({std::move(connection),std::move(text),std::move(token)});}
std::vector<SocketEvent> PluginWebSocketServer::poll(){std::lock_guard lock(impl_->mutex);std::vector<SocketEvent> result;while(!impl_->events.empty()){result.push_back(std::move(impl_->events.front()));impl_->events.pop_front();}return result;}
std::uint16_t PluginWebSocketServer::port()const{return impl_->boundPort.load();}
} // namespace velin
