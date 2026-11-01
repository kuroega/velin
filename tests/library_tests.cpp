// SPDX-License-Identifier: GPL-3.0-or-later
#include "velin/core.hpp"
#include "velin/library.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
std::string utf8(const std::filesystem::path& path){const auto text=path.u8string();return {reinterpret_cast<const char*>(text.data()),text.size()};}
struct Scratch {
  std::filesystem::path root=std::filesystem::current_path(),path;
  Scratch(){path=root/("velin-library-tests-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));check(std::filesystem::create_directory(path),"Create isolated test directory");}
  ~Scratch(){if(path.parent_path()==root&&path.filename().string().starts_with("velin-library-tests-")){std::error_code status;std::filesystem::remove_all(path,status);}}
};
void write(const std::filesystem::path& file,std::string_view text){std::ofstream stream(file,std::ios::binary);stream.write(text.data(),static_cast<std::streamsize>(text.size()));check(static_cast<bool>(stream),"Write test fixture");}

void history(const std::filesystem::path& root){
  velin::HistoryStore store(root/"history");check(store.load().empty()&&store.error().empty(),"Missing history starts empty");
  velin::HistoryEntry first{"C:\\Media\\\xe6\x98\xa0\xe7\x94\xbb.mkv","A film\nwith\tmetadata",100,20.5,120,0,false};
  check(store.record(first),"Record Unicode history");check(store.record(first),"Update existing history");
  auto rows=store.load();check(rows.size()==1&&rows[0].playCount==2&&rows[0].title==first.title,"History count and escaped metadata round trip");
  check(store.setFavorite(first.path,true),"Favorite history entry");
  first.lastPlayed=200;first.position=60;check(store.record(first,false),"Update position without counting a new play");
  rows=store.load();check(rows[0].playCount==2&&rows[0].favorite&&rows[0].position==60,"Position update preserves count and favorite");
  check(store.record({"https://example.test/video?a=1&b=2","Another Video",300,10,30,0,false}),"Record stream URL");
  check(store.search("ANOTHER example").size()==1,"Case-insensitive multi-term title/path search");
  check(store.search("",velin::HistorySort::Recent,true).size()==1,"Favorites-only history filter");
  check(store.search("",velin::HistorySort::MostPlayed).front().path==first.path,"Most-played history sorting");
  check(store.search("",velin::HistorySort::Duration).front().path==first.path,"Duration history sorting");
  check(store.pruneBefore(250),"Prune old nonfavorite history");check(store.load().size()==2,"Pruning preserves favorites");
  check(store.pruneBefore(250,false),"Prune old favorites explicitly");check(store.load().size()==1,"Old favorite removed when requested");
  check(store.remove("https://example.test/video?a=1&b=2")&&store.load().empty(),"Remove history entry");
  std::vector<velin::HistoryEntry> many;for(int i=0;i<1000;++i)many.push_back({"file"+std::to_string(i)+".mkv","Title "+std::to_string(i),i,0,0,1,i==0});
  check(store.save(many)&&store.load().size()==1000,"History has no recent-file count limit");
  check(store.clear(true)&&store.load().size()==1,"Clear history while preserving favorites");
  check(store.clear()&&store.load().empty(),"Clear all history");
  write(root/"history"/"history.tsv","# corrupt entries are ignored\nentry\tbad\ttitle\t-1\t0\t0\t1\t0\nentry\tbad\ttitle\t1\tnan\t2\t1\t0\nentry\t%00evil\ttitle\t1\t0\t0\t1\t0\n");
  check(store.load().empty(),"Malformed and NUL-injected history records are rejected");
  first.position=std::numeric_limits<double>::infinity();check(!store.record(first),"Non-finite history data is not persisted");
}

void playlists(const std::filesystem::path& root){
  velin::PlaylistStore store(root/"session");std::filesystem::create_directory(root/"media");
  const auto unicode=root/"media"/std::filesystem::path(std::u8string(u8"Movie \u96fb\u5f71 & one.mkv"));write(unicode,"fixture");
  velin::PlaylistDocument document{{{utf8(unicode),"A & B <film>",120.5},{"https://example.test/stream?id=1&token=2","Network \"stream\"",-1}},1,42.75};
  for(const auto* suffix:{".m3u8",".pls",".xspf"}){
    const auto file=root/(std::string("list")+suffix);check(store.save(file,document),"Save supported playlist format");
    const auto loaded=store.load(file);check(store.error().empty()&&loaded.entries.size()==2,"Load supported playlist format");
    check(loaded.entries[0].path==document.entries[0].path&&loaded.entries[0].title==document.entries[0].title,"Unicode local path and escaped title round trip");
    check(std::abs(loaded.entries[0].duration-120.5)<.001&&loaded.entries[1].path==document.entries[1].path,"Duration and stream URL round trip");
  }
  write(root/"relative.m3u8","\xef\xbb\xbf#EXTM3U\r\n#EXTINF:12.5,Relative movie\r\nmedia/one.mp4\r\nhttps://example.test/stream\r\n");
  const auto relative=store.load(root/"relative.m3u8");check(relative.entries.size()==2&&relative.entries[0].path==utf8((root/"media"/"one.mp4").lexically_normal()),"Relative playlist paths resolve against playlist directory");
  write(root/"numbered.pls","[playlist]\nTitle10=Ten\nFile10=ten.mp4\nFile2=two.mp4\nLength2=2\nNumberOfEntries=999\n");
  const auto numbered=store.load(root/"numbered.pls");check(numbered.entries.size()==2&&velin::mediaTitle(numbered.entries[0].path)=="two.mp4","PLS entries use numeric order and do not trust declared count");
  check(store.saveSession(document),"Save playlist session");const auto restored=store.loadSession();check(restored.entries.size()==2&&restored.entries[0].path==document.entries[0].path&&restored.currentIndex==1&&restored.position==42.75,"Session restores ordered entries, selection, and position");
  auto invalid=document;invalid.entries[0].path="file.mp4\n#EXTINF:0,injected";check(!store.save(root/"invalid.m3u8",invalid),"Playlist serialization rejects line injection");
  write(root/"external.xspf","<!DOCTYPE playlist [<!ENTITY leak SYSTEM \"file:///C:/secret\">]><playlist><trackList><track><location>&leak;</location></track></trackList></playlist>");
  check(store.load(root/"external.xspf").entries.empty()&&!store.error().empty(),"XSPF does not resolve external entities");
  write(root/"broken.xspf","<playlist><trackList><track><location>one.mp4</location></trackList></playlist>");
  check(store.load(root/"broken.xspf").entries.empty()&&!store.error().empty(),"Malformed XSPF track is rejected");
  check(!store.save(root/"unsupported.txt",document),"Unknown playlist output formats report an error");
}

void settings(const std::filesystem::path& root){
  velin::SettingsStore store(root/"preferences");velin::Preferences value;std::string error;
  check(velin::preferenceFields().size()>110,"Comprehensive preference metadata is available");
  check(velin::setPreferenceValue(value,"theme","light",error)&&value.theme==velin::Theme::Light,"Typed theme preference");
  check(velin::setPreferenceValue(value,"launchAction","restore-session",error),"Typed launch preference");
  check(velin::setPreferenceValue(value,"networkProxy","http://127.0.0.1:8080",error),"Network preference");
  check(velin::setPreferenceValue(value,"updateManifestUrl","https://example.test/releases.json",error),"Release URL preference");
  check(velin::setPreferenceValue(value,"checkForUpdates","1",error),"Update policy preference");
  check(velin::setPreferenceValue(value,"audioEqualizer","1,2,3,4,5,6,7,8,9,10",error),"Equalizer array setter");
  check(velin::setPreferenceValue(value,"audioEqualizer9","-3.5",error)&&value.audioEqualizer[9]==-3.5,"Individual equalizer band setter");
  check(!velin::setPreferenceValue(value,"audioEqualizer","1,2",error),"Incomplete equalizer is rejected");
  check(!velin::setPreferenceValue(value,"subtitleColor","red",error),"Subtitle color validation");
  check(velin::setPreferenceValue(value,"subtitleColor","#AA112233",error),"Explicit subtitle color");
  check(velin::setPreferenceValue(value,"keyBindings","control+shift+o=open-folder\nSpace=cycle pause",error),"Custom input map setter");
  check(value.keyBindings.contains("Ctrl+Shift+O")&&value.keyBindings.contains("Space"),"Input keys are normalized");
  check(!velin::setPreferenceValue(value,"keyBindings","Ctrl+O=open\nControl+O=another",error),"Duplicate normalized keys are rejected");
  check(!velin::setPreferenceValue(value,"keyBindings","Alt+O=open-files",error),"Unsupported system shortcuts are rejected");
  check(!velin::setPreferenceValue(value,"keyBindings","WheelUp=open-files",error),"Unsupported mouse shortcuts are rejected");
  check(velin::setPreferenceValue(value,"keyBindings","Ctrl+[=open-files\nF12=play-pause",error),"Dispatchable punctuation and function shortcuts are accepted");
  check(velin::setPreferenceValue(value,"mpvOptions","deband=yes\nsub-font=Example Font",error),"Generic mpv option map");
  check(!velin::setPreferenceValue(value,"mpvOptions","bad option=value",error),"Malformed mpv option names are rejected");
  check(velin::setPreferenceValue(value,"mpv.brightness","25",error)&&value.mpvOptions["brightness"]=="25","Live video adjustment maps to mpv option");
  check(!velin::setPreferenceValue(value,"mpv.brightness","999",error),"Video adjustment bounds enforced");
  value.screenshotDirectory="C:\\Images\\\xe6\x98\xa0\xe7\x94\xbb";value.extraMpvOptions="profile=gpu-hq\ncache=yes";value.historyWidth=1200;value.windowX=-1920;
  check(store.save(value),"Save comprehensive preferences");const auto read=store.load();
  for(const auto& field:velin::preferenceFields())check(velin::preferenceValue(value,field.key)==velin::preferenceValue(read,field.key),("Preference round trip: "+field.key).c_str());
  check(read.keyBindings==value.keyBindings&&read.audioEqualizer==value.audioEqualizer,"Typed maps and arrays round trip");
  check(velin::normalizeKeyBinding(" Shift + CONTROL + o ")=="Ctrl+Shift+O","Modifier normalization");
  check(velin::normalizeKeyBinding("Ctrl+Ctrl+O").empty()&&velin::normalizeKeyBinding("Ctrl+").empty(),"Malformed key chords rejected");
  const std::map<std::string,std::string> shortcuts{{"Ctrl+O","disabled"},{"F2","velin:open-files"},{"F3","open-url"}};
  check(velin::shortcutKeys("Ctrl+O","open-files",shortcuts)==std::vector<std::string>{"F2"},"Overridden default disappears and replacement appears in menu");
  check(velin::shortcutKeys("Ctrl+L","open-url",shortcuts)==std::vector<std::string>({"Ctrl+L","F3"}),"Unchanged default and additional shortcut both appear");
  check(velin::shortcutKeys("", "open-files", {{"Alt+O","open-files"}}).empty(),"Legacy unsupported bindings are not advertised as usable");
  value.subtitleSize=std::numeric_limits<double>::quiet_NaN();value.audioEqualizer[0]=1000;value.sidebarWidth=-1;
  const auto normalized=velin::normalizePreferences(value);check(normalized.subtitleSize==55&&normalized.audioEqualizer[0]==24&&normalized.sidebarWidth==180,"Comprehensive preferences normalize malformed in-memory values");
}

void plistImport(const std::filesystem::path& root){
  const auto xml=root/"iina.xml";
  write(xml,"<?xml version=\"1.0\" encoding=\"UTF-8\"?><!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\"><plist version=\"1.0\"><dict><key>softVolume</key><real>82.5</real><key>pauseWhenMinimized</key><true/><key>hardwareDecoder</key><integer>2</integer><key>subTextFont</key><string>A &amp; B</string><key>themeMaterial</key><integer>2</integer><key>platformOnly</key><array><string>ignored</string></array></dict></plist>");
  velin::Preferences preferences;std::string report;check(velin::importIinaPreferences(xml,preferences,report),"Import XML IINA property list");
  check(preferences.volume==82.5&&preferences.pauseWhenMinimized&&preferences.hardwareDecoding=="auto-copy"&&preferences.subtitleFont=="A & B"&&preferences.theme==velin::Theme::Light,"IINA preference mappings preserve typed values");
  const std::string hex="62706c6973743030d5010203040506070809065f100f68617264776172654465636f6465725f101270617573655768656e4d696e696d697a65645a736f6674566f6c756d655b73756254657874466f6e745d7468656d654d6174657269616c1002092340546000000000005c4578616d706c6520466f6e740813253a45515f61626b0000000000000101000000000000000a00000000000000000000000000000078";
  std::string binary;for(std::size_t i=0;i<hex.size();i+=2)binary+=static_cast<char>(std::stoi(hex.substr(i,2),nullptr,16));write(root/"iina.plist",binary);
  preferences={};check(velin::importIinaPreferences(root/"iina.plist",preferences,report),"Import native binary IINA property list");
  check(preferences.volume==81.5&&preferences.pauseWhenMinimized&&preferences.hardwareDecoding=="auto-copy"&&preferences.subtitleFont=="Example Font"&&preferences.theme==velin::Theme::Light,"Binary property-list values and reference reuse are parsed correctly");
  binary.back()=static_cast<char>(0xff);write(root/"invalid.plist",binary);const auto before=preferences.volume;
  check(!velin::importIinaPreferences(root/"invalid.plist",preferences,report)&&preferences.volume==before,"Malformed binary plist does not partially modify preferences");
  write(root/"entity.xml","<!DOCTYPE plist [<!ENTITY injected SYSTEM \"file:///C:/private\">]><plist><dict><key>subTextFont</key><string>&injected;</string></dict></plist>");
  check(!velin::importIinaPreferences(root/"entity.xml",preferences,report),"External entity declarations are rejected");
  write(root/"unknown.xml","<plist><dict><key>unknown</key><integer>1</integer></dict></plist>");
  check(!velin::importIinaPreferences(root/"unknown.xml",preferences,report)&&report.find("no supported")!=std::string::npos,"Unsupported preference files produce an informative report");
}
}

int main(){try{Scratch scratch;history(scratch.path);playlists(scratch.path);settings(scratch.path);plistImport(scratch.path);std::cout<<"All library tests passed.\n";return 0;}catch(const std::exception& error){std::cerr<<"Library test failure: "<<error.what()<<'\n';return 1;}}
