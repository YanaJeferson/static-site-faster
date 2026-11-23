#define NATIVE_TEST 1
#include "engine.cpp"
int main(){
  LinkEnv env; string base="b"; const string dir="", src="s"; env.base=&base; env.currentDir=&dir; env.currentSrc=&src;
  unordered_map<string,int> by; StringSet st; unordered_set<string> br;
  env.bySource=&by; env.statics=&st; env.broken=&br;
  unordered_map<string, unordered_map<string_view,string>> cache; env.cache=&cache;
  auto it=cache.emplace("", unordered_map<string_view,string>()).first; env.currentCache=&it->second;
  string s="Stone to schema signal careful_him_slow ***one *north* list*** way.\nPut **page** cache thread **remember *follow* lantern** plain our.";
  string out; inlineInto(out,s,env);
  printf("%s\n",out.c_str());
  return 0;
}
