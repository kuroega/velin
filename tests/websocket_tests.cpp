// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/socket_compat.hpp"
#include "velin/plugin_websocket.hpp"
#include <chrono>
#include <iostream>
#include <thread>
#include <stdexcept>
#include <system_error>

namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
struct Socket{SOCKET value=INVALID_SOCKET;~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}};
void sendAll(SOCKET socket,const std::string& text){size_t sent=0;while(sent<text.size()){const int count=static_cast<int>(send(socket,text.data()+sent,static_cast<int>(text.size()-sent),0));check(count>0,"Client send failed");sent+=static_cast<size_t>(count);}}
std::string receive(SOCKET socket,size_t length){std::string result(length,'\0');size_t got=0;while(got<length){const int count=static_cast<int>(recv(socket,result.data()+got,static_cast<int>(length-got),0));check(count>0,"Client receive failed");got+=static_cast<size_t>(count);}return result;}
std::string masked(unsigned opcode,const std::string& text,bool final=true){std::string result(1,static_cast<char>((final?128:0)|opcode));check(text.size()<126,"Fixture frame too large");result+=static_cast<char>(128|text.size());result+="abcd";for(size_t i=0;i<text.size();++i)result+=static_cast<char>(text[i]^"abcd"[i%4]);return result;}
velin::SocketEvent waitEvent(velin::PluginWebSocketServer& server,const std::string& name){const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(3);while(std::chrono::steady_clock::now()<deadline){for(auto& event:server.poll())if(event.event==name)return event;std::this_thread::sleep_for(std::chrono::milliseconds(5));}throw std::runtime_error("Timed out waiting for "+name);}
void connectClient(Socket& client,unsigned short port){client.value=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(client.value!=INVALID_SOCKET,"Create test client");velin::net::setReceiveTimeout(client.value,3000);velin::net::suppressSigpipe(client.value);sockaddr_in address{};address.sin_family=AF_INET;address.sin_port=htons(port);address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);if(connect(client.value,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0)throw std::system_error(velin::net::lastError(),std::system_category(),"Connect test client");}
std::string upgrade(SOCKET socket,const std::string& origin="http://localhost"){sendAll(socket,"GET /fixture HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive, Upgrade\r\nUpgrade: websocket\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nOrigin: "+origin+"\r\n\r\n");std::string response;while(response.find("\r\n\r\n")==std::string::npos){response+=receive(socket,1);check(response.size()<16384,"Oversized handshake response");}return response;}
}
int main(){try{
    velin::PluginWebSocketServer server;server.create({{"port",0},{"path","/fixture"},{"allowedOrigins",{"http://localhost"}}});server.start();check(server.port()!=0,"Ephemeral port assigned");
    Socket client;connectClient(client,server.port());const auto response=upgrade(client.value);
    check(response.find("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")!=std::string::npos,"RFC6455 handshake digest");
    const auto connection=waitEvent(server,"newConnection").args[0].get<std::string>();
    sendAll(client.value,masked(1,"hel",false)+masked(9,"ping")+masked(0,"lo"));
    check(receive(client.value,6)==std::string("\x8a\x04ping",6),"Interleaved ping/pong");
    const auto message=waitEvent(server,"message");check(message.args[1]["text"]=="hello","Fragmented text assembly");
    server.sendText(connection,"world","sent-1");check(receive(client.value,7)==std::string("\x81\x05world",7),"Unmasked server text frame");check(waitEvent(server,"sent").args[1]==true,"Send completion");
    server.sendText("missing","ignored","sent-2");check(waitEvent(server,"sent").args[2]=="no_connection","Missing client completion");
    sendAll(client.value,std::string("\x81\x01x",3));check(waitEvent(server,"connectionState").args[1]=="failed","Reject unmasked client frames");
    server.stop();server.create({{"port",0}});server.start();check(server.port()!=0,"Server restart");server.stop();
    std::cout<<"PASS RFC6455 handshake, masked frames, fragmentation, ping/pong, send completion, protocol rejection and restart\n";return 0;
}catch(const std::system_error& error){std::cerr<<error.what()<<"\n";return error.code().value()==velin::net::kAccessDenied?77:1;}catch(const std::exception& error){const int code=velin::net::lastError();std::cerr<<"FAIL "<<error.what()<<" (socket error "<<code<<")\n";return code==velin::net::kAccessDenied?77:1;}}
