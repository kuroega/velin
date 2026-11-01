// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/library.hpp"
#include "velin/core.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iomanip>
#include <map>
#include <limits>
#include <type_traits>
#include <sstream>
#include <system_error>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace velin {
namespace {
constexpr std::uintmax_t maximumFileBytes = 256ull * 1024 * 1024;
std::filesystem::path pathFromUtf8(std::string_view value) {
  return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(value.data()),value.size()));
}
std::string utf8Path(const std::filesystem::path& value) {
  const auto text=value.u8string(); return {reinterpret_cast<const char*>(text.data()),text.size()};
}
std::string trim(std::string_view text) {
  const auto begin=text.find_first_not_of(" \r\n\t");
  return begin==std::string_view::npos?std::string{}:std::string(text.substr(begin,text.find_last_not_of(" \r\n\t")-begin+1));
}
std::string lower(std::string_view text) {
#ifdef _WIN32
  const auto wide=fromUtf8(text);
  if (!wide.empty()) {
    const int length=LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,wide.data(),static_cast<int>(wide.size()),nullptr,0,nullptr,nullptr,0);
    if(length>0) { std::wstring out(static_cast<std::size_t>(length),L'\0'); LCMapStringEx(LOCALE_NAME_INVARIANT,LCMAP_LOWERCASE,wide.data(),static_cast<int>(wide.size()),out.data(),length,nullptr,nullptr,0); return toUtf8(out); }
  }
#endif
  std::string out(text); for(auto& ch:out) if(ch>='A' && ch<='Z') ch=static_cast<char>(ch+32); return out;
}
bool url(std::string_view path) { return path.find("://")!=std::string_view::npos; }
std::string pathKey(std::string_view path) {
#ifdef _WIN32
  if (!url(path)) { auto result=lower(path); std::replace(result.begin(),result.end(),'\\','/'); return result; }
#endif
  return std::string(path);
}
bool validText(std::string_view text, bool empty=true) {
  return (empty || !text.empty()) && text.size()<=1024*1024 && text.find('\0')==std::string_view::npos && (text.empty() || !fromUtf8(text).empty());
}
int hex(char ch) {
  if(ch>='0'&&ch<='9')return ch-'0';
  if(ch>='A'&&ch<='F')return ch-'A'+10;
  if(ch>='a'&&ch<='f')return ch-'a'+10;
  return -1;
}
std::string escape(std::string_view text) {
  constexpr char digits[]="0123456789ABCDEF"; std::string output;
  for(unsigned char ch:text) { if(ch<0x20 || ch=='%' || ch==0x7f) { output+='%';output+=digits[ch>>4];output+=digits[ch&15]; } else output+=static_cast<char>(ch); }
  return output;
}
std::string unescape(std::string_view text) {
  std::string output;
  for(std::size_t i=0;i<text.size();++i) {
    if(text[i]=='%'&&i+2<text.size()&&hex(text[i+1])>=0&&hex(text[i+2])>=0) { output+=static_cast<char>((hex(text[i+1])<<4)|hex(text[i+2]));i+=2; }
    else output+=text[i];
  }
  return output;
}
std::vector<std::string> split(std::string_view text,char separator) {
  std::vector<std::string> output; std::size_t start=0;
  while(start<=text.size()) {const auto end=text.find(separator,start);output.emplace_back(text.substr(start,end==std::string_view::npos?text.size()-start:end-start));if(end==std::string_view::npos)break;start=end+1;}
  return output;
}
template<class T> bool number(std::string_view text,T& value) {
  const auto clean=trim(text); T parsed{}; const auto result=std::from_chars(clean.data(),clean.data()+clean.size(),parsed);
  if(result.ec!=std::errc{}||result.ptr!=clean.data()+clean.size())return false;
  if constexpr(std::is_floating_point_v<T>) if(!std::isfinite(parsed))return false;
  value=parsed;return true;
}
std::string numeric(double value) {std::ostringstream output;output.imbue(std::locale::classic());output<<std::setprecision(17)<<value;return output.str();}
bool read(const std::filesystem::path& file,std::string& output,std::string& error,bool missingAllowed=false) {
  error.clear(); std::error_code status;
  const auto size=std::filesystem::file_size(file,status);
  if(status) { if(missingAllowed && !std::filesystem::exists(file)) {output.clear();return true;} error="Cannot read "+utf8Path(file)+": "+status.message();return false; }
  if(size>maximumFileBytes) {error="Library file exceeds the 256 MiB input safety limit.";return false;}
  std::ifstream input(file,std::ios::binary); if(!input) {error="Cannot open "+utf8Path(file);return false;}
  output.assign(std::istreambuf_iterator<char>(input),{});
  if(input.bad()) {error="Cannot finish reading "+utf8Path(file);return false;}
  if(output.starts_with("\xef\xbb\xbf"))output.erase(0,3);
  return true;
}
std::string xmlEscape(std::string_view text) {
  std::string out;for(char ch:text) switch(ch) {case '&':out+="&amp;";break;case '<':out+="&lt;";break;case '>':out+="&gt;";break;case '"':out+="&quot;";break;case '\'':out+="&apos;";break;default:out+=ch;break;}return out;
}
std::string xmlDecode(std::string_view text) {
  std::string out;
  for(std::size_t i=0;i<text.size();++i) {
    if(text[i]!='&') {out+=text[i];continue;}
    const auto end=text.find(';',i+1);if(end==std::string_view::npos||end-i>16)return{};
    const auto entity=text.substr(i+1,end-i-1);
    if(entity=="amp")out+='&';else if(entity=="lt")out+='<';else if(entity=="gt")out+='>';else if(entity=="quot")out+='"';else if(entity=="apos")out+='\'';
    else if(entity.starts_with('#')) {
      std::uint32_t code=0;const bool hexadecimal=entity.size()>1&&(entity[1]=='x'||entity[1]=='X');const auto digits=entity.substr(hexadecimal?2:1);
      const auto parsed=std::from_chars(digits.data(),digits.data()+digits.size(),code,hexadecimal?16:10);
      if(parsed.ec!=std::errc{}||parsed.ptr!=digits.data()+digits.size()||code==0||code>0x10ffff||(code>=0xd800&&code<=0xdfff))return{};
      if(code<0x80)out+=static_cast<char>(code);
      else if(code<0x800){out+=static_cast<char>(0xc0|(code>>6));out+=static_cast<char>(0x80|(code&63));}
      else if(code<0x10000){out+=static_cast<char>(0xe0|(code>>12));out+=static_cast<char>(0x80|((code>>6)&63));out+=static_cast<char>(0x80|(code&63));}
      else{out+=static_cast<char>(0xf0|(code>>18));out+=static_cast<char>(0x80|((code>>12)&63));out+=static_cast<char>(0x80|((code>>6)&63));out+=static_cast<char>(0x80|(code&63));}
    } else return{};
    i=end;
  }
  return out;
}
std::string tagValue(std::string_view input,std::string_view tag) {
  const auto opening="<"+std::string(tag)+">",closing="</"+std::string(tag)+">";
  const auto start=input.find(opening);if(start==std::string_view::npos)return{};
  const auto end=input.find(closing,start+opening.size());if(end==std::string_view::npos)return{};
  return xmlDecode(input.substr(start+opening.size(),end-start-opening.size()));
}
std::string resolvePath(std::string path,const std::filesystem::path& playlist) {
  if(path.starts_with("file://")) {
    path=unescape(std::string_view(path).substr(7));
#ifdef _WIN32
    if(path.starts_with("/" )&&path.size()>2&&path[2]==':')path.erase(0,1);
    else if(!path.starts_with('/')&&!path.starts_with('\\'))path="//"+path;
#endif
  } else if(url(path))return path;
  auto local=pathFromUtf8(path);
  if(local.is_relative())local=std::filesystem::absolute(playlist).parent_path()/local;
  return utf8Path(local.lexically_normal());
}
std::string uriForPath(std::string_view path) {
  if(url(path))return std::string(path);
  auto native=utf8Path(std::filesystem::absolute(pathFromUtf8(path)).lexically_normal());
  std::replace(native.begin(),native.end(),'\\','/');
  constexpr char digits[]="0123456789ABCDEF";std::string result;
  for(unsigned char ch:native) {if((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||ch=='/'||ch==':'||ch=='-'||ch=='_'||ch=='.'||ch=='~')result+=static_cast<char>(ch);else{result+='%';result+=digits[ch>>4];result+=digits[ch&15];}}
  if(result.starts_with("//"))return "file:"+result;
  return "file://"+std::string(result.starts_with('/')?"":"/")+result;
}
bool validEntry(const PlaylistEntry& entry) {return validText(entry.path,false)&&validText(entry.title)&&entry.path.find_first_of("\r\n")==std::string::npos&&entry.title.find_first_of("\r\n")==std::string::npos;}
}

bool writeTextFileAtomic(const std::filesystem::path& file,std::string_view text,std::string& error) {
  error.clear();std::error_code status;
  if(!file.parent_path().empty())std::filesystem::create_directories(file.parent_path(),status);
  if(status){error="Cannot create data directory: "+status.message();return false;}
  static std::atomic<std::uint64_t> sequence{0};
  const auto nonce=std::chrono::steady_clock::now().time_since_epoch().count();
  auto temporary=file;temporary+=".tmp-"+std::to_string(nonce)+"-"+std::to_string(sequence++);
#ifdef _WIN32
  HANDLE handle=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
  if(handle==INVALID_HANDLE_VALUE){error="Cannot create temporary data file (Windows error "+std::to_string(GetLastError())+").";return false;}
  bool okay=true;std::size_t written=0;
  while(written<text.size()) {const auto amount=static_cast<DWORD>(std::min<std::size_t>(text.size()-written,1024*1024));DWORD actual=0;if(!WriteFile(handle,text.data()+written,amount,&actual,nullptr)||actual==0){okay=false;break;}written+=actual;}
  if(okay)okay=FlushFileBuffers(handle)!=FALSE;
  CloseHandle(handle);
  if(okay)okay=MoveFileExW(temporary.c_str(),file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
  if(!okay){error="Cannot atomically replace data file (Windows error "+std::to_string(GetLastError())+").";std::filesystem::remove(temporary,status);}
  return okay;
#else
  const int handle=::open(temporary.c_str(),O_CREAT|O_EXCL|O_WRONLY,0600);
  if(handle<0){error="Cannot create temporary data file.";return false;}
  bool okay=true;std::size_t written=0;
  while(written<text.size()){const auto actual=::write(handle,text.data()+written,text.size()-written);if(actual<=0){okay=false;break;}written+=static_cast<std::size_t>(actual);}
  if(okay)okay=::fsync(handle)==0;::close(handle);
  if(okay){std::filesystem::rename(temporary,file,status);okay=!status;}
  if(!okay){error="Cannot atomically replace data file.";std::filesystem::remove(temporary,status);}return okay;
#endif
}

HistoryStore::HistoryStore(std::filesystem::path directory):directory_(std::move(directory)){}
const std::string& HistoryStore::error() const{return error_;}
std::vector<HistoryEntry> HistoryStore::load() const {
  std::string text;if(!read(directory_/"history.tsv",text,error_,true))return{};
  std::vector<HistoryEntry> entries;std::map<std::string,std::size_t> indices;
  for(const auto& line:split(text,'\n')) {
    const auto cells=split(line,'\t');if(cells.size()!=8||cells[0]!="entry")continue;
    HistoryEntry entry;entry.path=unescape(cells[1]);entry.title=unescape(cells[2]);int favorite=0;
    if(!validText(entry.path,false)||!validText(entry.title)||!number(cells[3],entry.lastPlayed)||entry.lastPlayed<0||!number(cells[4],entry.position)||entry.position<0||!number(cells[5],entry.duration)||entry.duration<0||!number(cells[6],entry.playCount)||!number(cells[7],favorite)||(favorite!=0&&favorite!=1))continue;
    entry.favorite=favorite!=0;if(entry.duration>0)entry.position=std::min(entry.position,entry.duration);
    const auto key=pathKey(entry.path);const auto found=indices.find(key);
    if(found==indices.end()){indices.emplace(key,entries.size());entries.push_back(std::move(entry));}
    else if(entry.lastPlayed>entries[found->second].lastPlayed)entries[found->second]=std::move(entry);
  }
  return entries;
}
bool HistoryStore::save(const std::vector<HistoryEntry>& entries) const {
  std::ostringstream output;output.imbue(std::locale::classic());output<<std::setprecision(17)<<"# Velin history v1\n";
  for(const auto& entry:entries) {
    if(!validText(entry.path,false)||!validText(entry.title)||entry.lastPlayed<0||!std::isfinite(entry.position)||!std::isfinite(entry.duration)||entry.position<0||entry.duration<0){error_="History contains an invalid entry.";return false;}
    output<<"entry\t"<<escape(entry.path)<<'\t'<<escape(entry.title)<<'\t'<<entry.lastPlayed<<'\t'<<entry.position<<'\t'<<entry.duration<<'\t'<<entry.playCount<<'\t'<<entry.favorite<<'\n';
  }
  return writeTextFileAtomic(directory_/"history.tsv",output.str(),error_);
}
bool HistoryStore::record(const HistoryEntry& entry,bool countPlay) {
  auto entries=load();if(!error_.empty())return false;
  const auto key=pathKey(entry.path);auto found=std::find_if(entries.begin(),entries.end(),[&](const auto& item){return pathKey(item.path)==key;});
  HistoryEntry updated=entry;
  if(updated.lastPlayed<=0)updated.lastPlayed=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  if(found!=entries.end()) {
    updated.playCount=found->playCount+(countPlay&&found->playCount<std::numeric_limits<std::uint64_t>::max()?1:0);
    updated.favorite=found->favorite;if(updated.title.empty())updated.title=found->title;*found=std::move(updated);
  } else {if(updated.title.empty())updated.title=mediaTitle(updated.path);updated.playCount=countPlay?std::max<std::uint64_t>(1,entry.playCount):entry.playCount;entries.push_back(std::move(updated));}
  return save(entries);
}
std::vector<HistoryEntry> HistoryStore::search(std::string_view query,HistorySort sort,bool favoritesOnly) const {
  auto entries=load();const auto terms=split(lower(trim(query)),' ');
  std::erase_if(entries,[&](const auto& entry){if(favoritesOnly&&!entry.favorite)return true;const auto text=lower(entry.title+'\n'+entry.path);for(const auto& term:terms)if(!term.empty()&&text.find(term)==std::string::npos)return true;return false;});
  std::stable_sort(entries.begin(),entries.end(),[sort](const auto& a,const auto& b){switch(sort){case HistorySort::Title:{const auto left=lower(a.title),right=lower(b.title);if(left!=right)return left<right;break;}case HistorySort::MostPlayed:if(a.playCount!=b.playCount)return a.playCount>b.playCount;break;case HistorySort::Duration:if(a.duration!=b.duration)return a.duration>b.duration;break;case HistorySort::Recent:break;}return a.lastPlayed>b.lastPlayed;});
  return entries;
}
bool HistoryStore::remove(std::string_view path){auto entries=load();if(!error_.empty())return false;const auto key=pathKey(path);std::erase_if(entries,[&](const auto& entry){return pathKey(entry.path)==key;});return save(entries);}
bool HistoryStore::setFavorite(std::string_view path,bool favorite){auto entries=load();if(!error_.empty())return false;const auto key=pathKey(path);for(auto& entry:entries)if(pathKey(entry.path)==key)entry.favorite=favorite;return save(entries);}
bool HistoryStore::clear(bool keepFavorites){auto entries=load();if(!error_.empty())return false;if(keepFavorites)std::erase_if(entries,[](const auto& entry){return !entry.favorite;});else entries.clear();return save(entries);}
bool HistoryStore::pruneBefore(std::int64_t timestamp,bool keepFavorites){auto entries=load();if(!error_.empty())return false;std::erase_if(entries,[&](const auto& entry){return entry.lastPlayed<timestamp&&(!keepFavorites||!entry.favorite);});return save(entries);}

PlaylistStore::PlaylistStore(std::filesystem::path directory):directory_(std::move(directory)){}
const std::string& PlaylistStore::error() const{return error_;}
PlaylistDocument PlaylistStore::load(const std::filesystem::path& file) const {
  PlaylistDocument document;std::string text;if(!read(file,text,error_))return document;
  if(text.find('\0')!=std::string::npos || (!text.empty()&&fromUtf8(text).empty())){error_="Playlist is not valid UTF-8 text.";return document;}
  const auto suffix=lower(utf8Path(file.extension()));
  auto append=[&](std::string path,std::string title,double duration){if(!validText(path,false)||!validText(title))return;document.entries.push_back({resolvePath(std::move(path),file),std::move(title),std::isfinite(duration)&&duration>=0?duration:-1});};
  if(suffix==".pls"||lower(text.substr(0,std::min<std::size_t>(text.size(),10))).starts_with("[playlist]")) {
    std::map<int,PlaylistEntry> indexed;
    for(const auto& raw:split(text,'\n')) {
      const auto line=trim(raw);const auto equals=line.find('=');if(equals==std::string::npos)continue;
      const auto key=lower(std::string_view(line).substr(0,equals));const auto value=line.substr(equals+1);
      const auto digit=key.find_first_of("0123456789");if(digit==std::string::npos)continue;
      int index=0;if(!number(std::string_view(key).substr(digit),index)||index<1)continue;
      const auto kind=key.substr(0,digit);
      if(kind=="file")indexed[index].path=value;else if(kind=="title")indexed[index].title=value;else if(kind=="length")number(value,indexed[index].duration);
    }
    for(auto& [index,entry]:indexed){(void)index;append(std::move(entry.path),std::move(entry.title),entry.duration);}
  } else if(suffix==".xspf"||text.find("<playlist")!=std::string::npos) {
    if(text.find("<!DOCTYPE")!=std::string::npos||text.find("<!ENTITY")!=std::string::npos){error_="XSPF DTD and entity declarations are not supported.";return{};}
    std::size_t position=0;
    while((position=text.find("<track>",position))!=std::string::npos) {
      const auto end=text.find("</track>",position+7);if(end==std::string::npos){error_="Malformed XSPF track.";return{};}
      const std::string_view track(text.data()+position+7,end-position-7);double duration=-1;auto durationText=tagValue(track,"duration");if(number(durationText,duration))duration/=1000;
      append(tagValue(track,"location"),tagValue(track,"title"),duration);position=end+8;
    }
  } else {
    std::string title;double duration=-1;
    for(const auto& raw:split(text,'\n')) {
      const auto line=trim(raw);if(line.empty())continue;
      if(line.starts_with("#EXTINF:")){const auto comma=line.find(',');if(comma!=std::string::npos){number(std::string_view(line).substr(8,comma-8),duration);title=line.substr(comma+1);}continue;}
      if(line.front()=='#')continue;
      append(line,title,duration);title.clear();duration=-1;
    }
  }
  if(!document.entries.empty())document.currentIndex=0;
  return document;
}
bool PlaylistStore::save(const std::filesystem::path& file,const PlaylistDocument& document) const {
  error_.clear();for(const auto& entry:document.entries)if(!validEntry(entry)){error_="Playlist paths and titles must be valid UTF-8 without line breaks.";return false;}
  const auto suffix=lower(utf8Path(file.extension()));std::ostringstream out;out.imbue(std::locale::classic());out<<std::setprecision(17);
  if(suffix==".xspf") {
    out<<"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<playlist version=\"1\" xmlns=\"http://xspf.org/ns/0/\"><trackList>\n";
    for(const auto& entry:document.entries){out<<"<track><location>"<<xmlEscape(uriForPath(entry.path))<<"</location><title>"<<xmlEscape(entry.title)<<"</title>";if(std::isfinite(entry.duration)&&entry.duration>=0)out<<"<duration>"<<entry.duration*1000<<"</duration>";out<<"</track>\n";}out<<"</trackList></playlist>\n";
  } else if(suffix==".pls") {
    out<<"[playlist]\n";for(std::size_t i=0;i<document.entries.size();++i){const auto& entry=document.entries[i];out<<"File"<<i+1<<'='<<entry.path<<"\nTitle"<<i+1<<'='<<entry.title<<"\nLength"<<i+1<<'='<<(std::isfinite(entry.duration)?entry.duration:-1)<<'\n';}out<<"NumberOfEntries="<<document.entries.size()<<"\nVersion=2\n";
  } else if(suffix==".m3u8"||suffix==".m3u") {
    out<<"#EXTM3U\n";for(const auto& entry:document.entries)out<<"#EXTINF:"<<(std::isfinite(entry.duration)?entry.duration:-1)<<','<<entry.title<<'\n'<<entry.path<<'\n';
  } else {error_="Choose an M3U8, M3U, PLS, or XSPF playlist filename.";return false;}
  return writeTextFileAtomic(file,out.str(),error_);
}
PlaylistDocument PlaylistStore::loadSession() const {
  PlaylistDocument document;std::string text;if(!read(directory_/"session.tsv",text,error_,true))return document;
  for(const auto& line:split(text,'\n')) {
    const auto cells=split(line,'\t');if(cells.size()==2&&cells[0]=="current")number(cells[1],document.currentIndex);
    else if(cells.size()==2&&cells[0]=="position")number(cells[1],document.position);
    else if(cells.size()==4&&cells[0]=="item") {PlaylistEntry entry{unescape(cells[1]),unescape(cells[2]),-1};number(cells[3],entry.duration);if(validText(entry.path,false)&&validText(entry.title))document.entries.push_back(std::move(entry));}
  }
  if(document.currentIndex<0||document.currentIndex>=static_cast<std::int64_t>(document.entries.size()))document.currentIndex=document.entries.empty()?-1:0;
  if(!std::isfinite(document.position)||document.position<0)document.position=0;
  return document;
}
bool PlaylistStore::saveSession(const PlaylistDocument& document) const {
  std::ostringstream out;out.imbue(std::locale::classic());out<<"# Velin session v1\ncurrent\t"<<document.currentIndex<<"\nposition\t"<<numeric(std::isfinite(document.position)&&document.position>=0?document.position:0)<<'\n';
  for(const auto& entry:document.entries){if(!validText(entry.path,false)||!validText(entry.title)){error_="Session contains an invalid path or title.";return false;}out<<"item\t"<<escape(entry.path)<<'\t'<<escape(entry.title)<<'\t'<<numeric(std::isfinite(entry.duration)?entry.duration:-1)<<'\n';}
  return writeTextFileAtomic(directory_/"session.tsv",out.str(),error_);
}
} // namespace velin
