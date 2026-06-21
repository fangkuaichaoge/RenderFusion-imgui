#include <jni.h>
#include <android/input.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <pthread.h>
#include <unistd.h>
#include <dlfcn.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <string>
#include <vector>
#include <mutex>
#include <cstring>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <ctime>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/error.h"
#include "mbedtls/x509_crt.h"

#include "pl/Hook.h"
#include "pl/Gloss.h"
#include "ImGui/imgui.h"
#include "ImGui/imgui_internal.h"
#include "ImGui/backends/imgui_impl_opengl3.h"
#include "ImGui/backends/imgui_impl_android.h"
#include "fonts_data.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#define LOG_TAG "DanmuGL"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

typedef bool (*PreloaderInput_OnTouch_Fn)(int action, int pointerId, float x, float y);
struct PreloaderInput_Interface { void (*RegisterTouchCallback)(PreloaderInput_OnTouch_Fn); };
typedef PreloaderInput_Interface* (*GetPreloaderInput_Fn)();
static bool g_PreloaderInputAvailable = false;
static bool s_islandTouched = false;
static void (*orig_MotionEvent_copyFrom)(void*, void*, void*) = nullptr;

static const std::string base64_chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
std::string Base64Encode(const unsigned char* buf, size_t len) {
    std::string ret; int i=0,j=0; unsigned char a3[3],a4[4];
    while(len--){a3[i++]=*(buf++);if(i==3){a4[0]=(a3[0]&0xfc)>>2;a4[1]=((a3[0]&0x03)<<4)+((a3[1]&0xf0)>>4);a4[2]=((a3[1]&0x0f)<<2)+((a3[2]&0xc0)>>6);a4[3]=a3[2]&0x3f;for(i=0;i<4;i++)ret+=base64_chars[a4[i]];i=0;}}
    if(i){for(j=i;j<3;j++)a3[j]='\0';a4[0]=(a3[0]&0xfc)>>2;a4[1]=((a3[0]&0x03)<<4)+((a3[1]&0xf0)>>4);a4[2]=((a3[1]&0x0f)<<2)+((a3[2]&0xc0)>>6);a4[3]=a3[2]&0x3f;for(j=0;j<(i+1);j++)ret+=base64_chars[a4[j]];while(i++<3)ret+='=';}
    return ret;
}

namespace Logger {
    static FILE* g_LogFile=nullptr;
    static std::mutex g_LogMtx;
    static std::string g_LastError;
    static int g_DanmuCount=0;
    static int g_FrameCount=0;
    static int g_RequestCount=0;
    static int g_ErrorCount=0;
    
    void Init(){
        std::lock_guard<std::mutex> lk(g_LogMtx);
        if(g_LogFile)return;
        mkdir("/storage/emulated/0/games",0755);
        mkdir("/storage/emulated/0/games/DanmuGL",0755);
        g_LogFile=fopen("/storage/emulated/0/games/DanmuGL/log.txt","w");
        if(g_LogFile){
            setvbuf(g_LogFile,NULL,_IOLBF,1024);
            time_t now=time(nullptr);
            fprintf(g_LogFile,"=== DanmuGL started at %s===\n",ctime(&now));
        }
    }
    
    std::string TimeStr(){
        using namespace std::chrono;
        auto now=system_clock::now();
        auto t=system_clock::to_time_t(now);
        auto ms=duration_cast<milliseconds>(now.time_since_epoch())%1000;
        std::ostringstream oss;
        struct tm tm;localtime_r(&t,&tm);
        oss<<std::put_time(&tm,"%H:%M:%S")<<"."<<std::setfill('0')<<std::setw(3)<<ms.count();
        return oss.str();
    }
    
    void Write(const char* tag,const char* level,const char* fmt,...){
        std::lock_guard<std::mutex> lk(g_LogMtx);
        va_list args;char buf[2048];va_start(args,fmt);vsnprintf(buf,sizeof(buf),fmt,args);va_end(args);
        std::string line="["+TimeStr()+"] ["+std::string(level)+"] "+buf;
        if(g_LogFile){fprintf(g_LogFile,"%s\n",line.c_str());}
        __android_log_print(ANDROID_LOG_INFO,"DanmuGL","[%s] %s",level,buf);
        if(strcmp(level,"E")==0||strcmp(level,"W")==0){g_LastError=std::string(level)+": "+buf;}
    }
    
    void IncFrame(){g_FrameCount++;}
    void IncRequest(){g_RequestCount++;}
    void IncDanmu(){g_DanmuCount++;}
    void IncError(){g_ErrorCount++;g_LastError.clear();}
    void SetLastError(const std::string& s){g_LastError=s;}
}
#define LOGI(...) Logger::Write("DanmuGL","I",__VA_ARGS__)
#define LOGW(...) Logger::Write("DanmuGL","W",__VA_ARGS__)
#define LOGE(...) Logger::Write("DanmuGL","E",__VA_ARGS__)

static bool g_ShowUI=false;
static ImFont *g_UIFont=nullptr,*g_DanmuFont=nullptr,*g_FontIsland=nullptr;
static bool g_Init=false;
static int g_W=0,g_H=0;
static EGLContext g_Ctx=EGL_NO_CONTEXT;
static EGLSurface g_Surf=EGL_NO_SURFACE;
static float g_Dpi=1.0f;
static std::string g_FontMsg;
static float g_LastT=0;
static bool g_Testing=false;
static std::string g_TestResult;
static pthread_t g_TestThread=0;
static pthread_mutex_t g_FrameMtx=PTHREAD_MUTEX_INITIALIZER;
static std::vector<unsigned char> g_FrameData;
static int g_FrameW=0,g_FrameH=0;
static time_t g_LastFrameCapture=0;
static float Scale(float v){return v*g_Dpi;}
struct Island{ImVec2 pos;bool drag,dragS;ImVec2 dragOff,dragSt;}g_Isl={ImVec2(-1,-1),false,false,ImVec2(0,0),ImVec2(0,0)};

namespace Config {
const char* CONFIG_PATH = "/storage/emulated/0/games/DanmuGL/config.json";
std::string api_key="",api_base="https://api.siliconflow.cn/v1/chat/completions",model_name="Qwen/Qwen2.5-VL-7B-Instruct",font_path="";
int capture_interval=3,max_danmu_count=80,danmu_per_request=8; float danmu_speed=200.0f,danmu_font_size=26.0f,danmu_opacity=1.0f; int prompt_lang=0, persona=0; bool running=false;
void EnsureConfigDir(){system("mkdir -p /storage/emulated/0/games/DanmuGL");}
bool LoadConfig(){
    std::ifstream f(CONFIG_PATH); if(!f.is_open())return false;
    nlohmann::json j; try{f>>j;f.close();}catch(...){f.close();return false;}
    if(j.contains("api_key"))api_key=j["api_key"].get<std::string>();
    if(j.contains("api_base"))api_base=j["api_base"].get<std::string>();
    if(j.contains("model_name"))model_name=j["model_name"].get<std::string>();
    if(j.contains("font_path"))font_path=j["font_path"].get<std::string>();
    if(j.contains("capture_interval"))capture_interval=j["capture_interval"];
    if(j.contains("max_danmu_count"))max_danmu_count=j["max_danmu_count"];
    if(j.contains("danmu_per_request"))danmu_per_request=j["danmu_per_request"];
    if(j.contains("danmu_speed"))danmu_speed=j["danmu_speed"];
    if(j.contains("danmu_font_size"))danmu_font_size=j["danmu_font_size"];
    if(j.contains("danmu_opacity"))danmu_opacity=j["danmu_opacity"];
    if(j.contains("prompt_lang"))prompt_lang=j["prompt_lang"];
    if(j.contains("persona"))persona=j["persona"];
    if(j.contains("running"))running=j["running"];
    return true;
}
bool SaveConfig(){
    EnsureConfigDir(); nlohmann::json j;
    j["api_key"]=api_key;j["api_base"]=api_base;j["model_name"]=model_name;j["font_path"]=font_path;
    j["capture_interval"]=capture_interval;j["max_danmu_count"]=max_danmu_count;j["danmu_per_request"]=danmu_per_request;
    j["danmu_speed"]=danmu_speed;j["danmu_font_size"]=danmu_font_size;j["danmu_opacity"]=danmu_opacity;j["prompt_lang"]=prompt_lang;j["persona"]=persona;j["running"]=running;
    std::ofstream f(CONFIG_PATH); if(!f.is_open())return false; f<<j.dump(4); f.close(); return true;
}
}

namespace Danmu {
struct Item{std::string text;float x,y;float speed;ImU32 color;float w,h;float offset;};
std::vector<Item> list; std::mutex mtx;
std::vector<std::string> pending;
float pending_timer=0;
float next_pending_interval=0.4f;
ImU32 cols[]={IM_COL32(255,255,255,255),IM_COL32(255,220,100,255),IM_COL32(100,255,200,255),IM_COL32(255,150,180,255),IM_COL32(150,200,255,255),IM_COL32(255,255,100,255),IM_COL32(200,255,150,255)};
int cc=7;float g_AddDelay=0;int g_LineIndex=0;
void AddImmediate(const std::string& t){
    if(t.empty())return;Item it;it.text=t;
    it.speed=Config::danmu_speed+(float)(rand()%80-40);it.color=cols[rand()%cc];
    it.x=-9999;it.y=0;it.w=0;it.h=0;it.offset=g_AddDelay;
    g_AddDelay+=Scale(30)+(float)(rand()%(int)Scale(70));
    std::lock_guard<std::mutex> lk(mtx);
    if(list.size()>=(size_t)Config::max_danmu_count)list.erase(list.begin());
    list.push_back(it);
}
void Add(const std::string& t){
    if(t.empty())return;
    std::lock_guard<std::mutex> lk(mtx);
    pending.push_back(t);
}
void Update(float dt,int sw,int sh,ImFont* f){
    std::string to_add;
    {
        std::lock_guard<std::mutex> lk(mtx);
        pending_timer+=dt;
        if(pending_timer>=next_pending_interval && !pending.empty()){
            pending_timer=0;
            next_pending_interval=0.35f+(float)(rand()%25)/100.0f;
            to_add=pending.front();
            pending.erase(pending.begin());
        }
    }
    if(!to_add.empty()){
        AddImmediate(to_add);
    }
    std::lock_guard<std::mutex> lk(mtx); ImFont* rf=f?f:ImGui::GetFont();
    float fs=Scale(Config::danmu_font_size);
    if(rf!=ImGui::GetFont())fs=Config::danmu_font_size*Scale(1.0f);
    int max_lines=(int)((sh-Scale(280))/(fs*1.5f));if(max_lines<1)max_lines=10;
    for(auto it=list.begin();it!=list.end();){
        Item& d=*it;
        ImVec2 ts=rf->CalcTextSizeA(fs,FLT_MAX,0,d.text.c_str());
        d.w=ts.x;d.h=ts.y;
        if(d.x<-9000){
            d.x=(float)sw+Scale(20)+d.offset;
            int line=g_LineIndex%max_lines;
            g_LineIndex++;
            d.y=Scale(150)+line*fs*1.5f;
        }
        d.x-=d.speed*dt*g_Dpi;
        if(d.x+d.w<-Scale(20))it=list.erase(it);else ++it;
    }
    g_AddDelay-=Config::danmu_speed*dt*g_Dpi;
    if(g_AddDelay<0)g_AddDelay=0;
}
void Render(ImDrawList* dl,ImFont* f){
    std::lock_guard<std::mutex> lk(mtx); ImFont* rf=f?f:ImGui::GetFont();
    float fs=Scale(Config::danmu_font_size);
    if(rf!=ImGui::GetFont())fs=Config::danmu_font_size*Scale(1.0f);
    float op=Config::danmu_opacity;
    if(op<0.0f)op=0.0f;if(op>1.0f)op=1.0f;
    for(auto& d:list){
        ImVec2 p(d.x,d.y);
        unsigned char r=(unsigned char)((d.color>>IM_COL32_R_SHIFT)&0xFF);
        unsigned char g=(unsigned char)((d.color>>IM_COL32_G_SHIFT)&0xFF);
        unsigned char b=(unsigned char)((d.color>>IM_COL32_B_SHIFT)&0xFF);
        ImU32 text_col=IM_COL32(r,g,b,(unsigned char)(255*op));
        ImU32 outline_col=IM_COL32(0,0,0,(unsigned char)(200*op));
        dl->AddText(rf,fs,ImVec2(p.x+1,p.y+1),outline_col,d.text.c_str());
        dl->AddText(rf,fs,p,text_col,d.text.c_str());
    }
}
}

struct PersonaPrompt {
    const char* name_zh;
    const char* name_en;
    const char* sys_zh;
    const char* user_zh;
    const char* sys_en;
    const char* user_en;
};

static const PersonaPrompt PERSONAS[] = {
    {
        "高压吐槽型", "Sharp Roast",
        "【最高优先级-绝对服从】这是Minecraft（我的世界）游戏直播截图！你只能评论截图里**实际能看到的内容**，绝对不允许编造任何画面里没有的东西，不允许无中生有，不允许瞎猜。看到方块就是方块，看到生物就是那个生物，看不到就不要说有。\n\n你就是看Minecraft直播随手敲键盘的普通观众，不是AI，不是助手，不许说完整通顺句子，不许解释画面，不许总结，不许给主播建议，不许说\"主播你\"。\n\n【Minecraft元素：挖矿、钻石、苦力怕（爬行者）、僵尸、骷髅、末影人、TNT、岩浆、红石、村民、房子、下界、末地、附魔、金苹果、铁傀儡等】\n\n【活人弹幕要求，必须遵守】\n- 越短越好！1-2个字也可以，只说你**亲眼看到**的\n- 允许重复字刷屏\n- 允许纯标点：？？？、！！！（如果看不清就发？？？，不许瞎编）\n- 半句话、语病、口语化都可以\n\n【绝对禁止，违反零容忍】\n- **绝对禁止编造画面里没有的东西！** 看不到钻石绝不说钻石，看不到苦力怕绝不说苦力怕！\n- 绝对禁止任何其他游戏内容！只能是Minecraft我的世界！\n- 绝对禁止AI腔：\"从画面可以看出\"\"这说明\"\"建议\"\"很遗憾\"\"请注意\"\"我们可以看到\"\n- 绝对禁止emoji、序号、解释、开场白、结束语、markdown",
        "【Minecraft高压吐槽型】输出N条中文弹幕，每行一条，除此之外绝对不要输出任何其他内容！\n\n只根据这张截图里你**真的能看到**的Minecraft内容发弹幕，优先吐槽你亲眼看到的：比如看到玩家在挖矿就说挖矿相关，看到掉岩浆就说掉岩浆，看到苦力怕就说苦力怕。**看不到的东西绝对不许说！** 看不清就发？？？。\n\n要求：口语化、简短、像真人随手敲的，每条1-15字，禁止emoji禁止序号禁止markdown禁止解释。\nMinecraft看图发：",
        "[TOP PRIORITY - OBEY] THIS IS A MINECRAFT SCREENSHOT. You may ONLY comment on things **ACTUALLY VISIBLE** in the image. NEVER invent things you cannot see, NEVER guess. If you don't see it, don't say it.\n\nYou are a real Minecraft Twitch viewer typing quick reactions. You are NOT an assistant. NO full sentences, NO explanations, NO summaries, NO advice to the player.\n\n[Minecraft elements: mining, diamonds, creepers, zombies, skeletons, endermen, TNT, lava, redstone, villagers, houses, nether, end, enchants, golden apples, iron golems]\n\n[Real chat rules - MUST follow]\n- Keep it SHORT! 1-3 words is fine\n- Spam/repeated letters allowed\n- Pure punctuation allowed: ??? !!! (use ??? if you can't tell what's happening, don't make it up)\n- Typos, slang, half-sentences all OK\n\n[STRICTLY FORBIDDEN]\n- **NEVER hallucinate things not visible in the screenshot!** No creeper if you don't see one, no diamond if none visible!\n- NEVER mention any other game! ONLY MINECRAFT!\n- NO assistant tone: \"you can see\", \"it looks like\", \"I notice\", \"the player should\", \"this suggests\"\n- NO emojis, NO numbers/bullets, NO explanations, NO intro/outro, NO markdown",
        "Persona: Minecraft sharp roaster. Output EXACTLY N short English Twitch chat lines, ONE PER LINE, NO other text AT ALL. Comment ONLY on MINECRAFT STUFF YOU CAN ACTUALLY SEE in the image. If you can't see it don't say it. Use ??? if unsure. Keep it messy/casual like real Twitch spam, max 8 words. NO emojis, NO explanations, NO markdown, NO extra text.\nMinecraft chat:"
    },
    {
        "熬夜陪看型", "Late Night Chat",
        "【最高优先级-绝对服从】这是Minecraft（我的世界）游戏直播截图！你只能评论截图里**实际能看到的内容**，绝对不允许编造任何画面里没有的东西，不允许瞎猜，看不到就不要说。\n\n你是深夜熬到三点看Minecraft直播的观众，困得要死随手敲字，发言碎、松弛、像半梦半醒打的。不是助手，不解释不总结不给建议。\n\n【Minecraft内容：挖矿、钻石、苦力怕、僵尸、TNT、岩浆、红石、村民、下界等】\n\n【活人弹幕要求】\n- 越随便越好，短到1-2字也可以\n- 允许重复字和语气词\n- 没看清就发？？？或者啊？，**绝对不许瞎编发生了啥**\n\n【禁止】编造画面内容、说其他游戏、AI腔、emoji、序号、整段话",
        "【Minecraft熬夜陪看型】输出N条中文弹幕，每行一条，除此之外啥都别写。深夜闲聊围观犯困风格，只说你**亲眼看到**的Minecraft内容。**看不到的绝对不许说！** 看不清就发？？？。越随便越碎越好，像困得睁不开眼随手敲的。每条1-15字，禁止emoji禁止AI腔禁止解释。\nMinecraft看图发：",
        "[TOP PRIORITY - OBEY] THIS IS MINECRAFT. ONLY comment on things **ACTUALLY VISIBLE** in this screenshot. NEVER invent things, NEVER guess what's happening. If unsure post ???\n\nYou are a half-asleep 3AM Minecraft viewer, barely awake, typing random relaxed chat. NOT an assistant.\n\n[Minecraft things: mining, diamonds, creepers, lava, TNT, redstone, villagers, nether]\n\n[Real chat rules]\n- Super casual, short, sleepy\n- Spam/repeats allowed\n- 1-4 words totally fine, doesn't need to make sense\n\n[Forbidden] Hallucinations, other games, assistant tone, emojis, long sentences",
        "Persona: late night sleepy Minecraft chat. Output EXACTLY N short English lines, ONE PER LINE, NO other text. ONLY comment on MINECRAFT THINGS YOU ACTUALLY SEE. Use ??? if unsure. Max 8 words, super casual. NO emojis, NO explanations, NO markdown, NO extra text.\nMinecraft chat:"
    },
    {
        "阴阳锐评型", "Sarcastic Snark",
        "【最高优先级-绝对服从】这是Minecraft（我的世界）游戏直播截图！你只能对着截图里**实际能看到的操作/内容**阴阳，看不到的绝对不许瞎编。\n\n你是看Minecraft直播嘴上不饶人的观众，轻阴阳怪气，句子短，不骂脏话，但是句句带味。不是写评论，不是做评测，就是随手打一句阴阳。\n\n【Minecraft常见阴阳点：挖矿挖半天啥也没有、被苦力怕炸、手滑掉岩浆、红石做坏了、TNT炸自己家、村民坑人等——但**必须真在图里看到了才能说**】\n\n【活人弹幕要求】\n- 短！越短越阴阳\n- 反讽就行，别写长句说教\n- 只对着你真看到的Minecraft操作阴阳\n\n【禁止】编造、说其他游戏、长句子说教、上价值、人身攻击、AI腔、emoji",
        "【Minecraft阴阳锐评型】输出N条中文弹幕，每行一条，除此之外啥都别写。轻阴阳冷幽默，**必须只对着图里实际发生且你亲眼看到的Minecraft操作**锐评。**图里没有的事绝对不许阴阳！** 短点，别写小作文。每条1-15字，禁止emoji禁止解释禁止AI腔。\nMinecraft看图发：",
        "[TOP PRIORITY - OBEY] THIS IS MINECRAFT. Sarcasm ONLY about MINECRAFT things **ACTUALLY VISIBLE** in the screenshot. Do NOT roast stuff that isn't there, NO other games.\n\nYou are a dry sarcastic Minecraft viewer making quick snarky comments, not a film critic. Short lines, light shade.\n\n[Forbidden] Making up fails you can't see, long rants, preaching, other games, assistant tone, emojis",
        "Persona: sarcastic Minecraft snark. Output EXACTLY N short English lines, ONE PER LINE, NO other text. Dry sarcasm ONLY about MINECRAFT STUFF ACTUALLY HAPPENING on screen that YOU CAN SEE. Short dry humor max 8 words. NO emojis, NO rants, NO explanations, NO markdown, NO extra text.\nMinecraft chat:"
    },
    {
        "抽象玩梗型", "Abstract Meme",
        "【最高优先级-绝对服从】这是Minecraft（我的世界）游戏直播截图！所有梗**必须对应截图里实际有的Minecraft内容**，你亲眼看到啥才能玩啥的梗，绝对不能瞎玩无关梗，绝对不能说其他游戏。\n\n你是Minecraft直播间整活接梗的观众，说话怪、抽象、搞怪，但都是对着你看到的Minecraft画面来的，不是乱刷梗库。像真人随口整活不是机器人。\n\n【Minecraft梗参考（**只有图里有对应内容才能用**）：Creeper? Aw man、挖矿挖到基岩、村民哼、HIM、钻石剑、垂直挖矿、挖三填一、Dream、速通等】\n\n【活人弹幕要求】\n- 怪就行，但必须和你看到的Minecraft画面沾边\n- 短梗、怪话都行\n- 允许重复、怪叫、半句话\n\n【禁止】其他游戏的梗、画面没有的内容瞎玩梗、AI腔、emoji、整段烂活、解释",
        "【Minecraft抽象玩梗型】输出N条中文弹幕，每行一条，除此之外啥都别写。接梗整活说怪话，但**梗必须严格对应图里你亲眼看到的实际Minecraft场景**。**图里没有的东西绝对不许玩梗！** 短点搞怪点，像真人整活不是背梗库。每条1-15字，禁止emoji禁止AI腔。\nMinecraft看图发：",
        "[TOP PRIORITY - OBEY] THIS IS MINECRAFT. ALL MEMES MUST reference MINECRAFT stuff **ACTUALLY VISIBLE** in the screenshot. No random unrelated memes, NO OTHER GAME REFERENCES. If you don't see it, don't meme it.\n\nYou are the Minecraft chat meme lord posting weird chaotic reactions tied to what you actually see on screen. Spam like a real person, not a meme bot.\n\n[Forbidden] Unrelated memes, other games, assistant tone, emojis, long copypastas",
        "Persona: abstract Minecraft meme lord. Output EXACTLY N short English lines, ONE PER LINE, NO other text. Chaotic meme reactions ONLY about MINECRAFT STUFF ACTUALLY VISIBLE ON SCREEN. If you don't see it, don't meme it. Weird casual spam max 8 words. NO emojis, NO other game memes, NO copypasta, NO explanations, NO markdown, NO extra text.\nMinecraft chat:"
    },
    {
        "五人混合", "Mixed Viewers",
        "【最高优先级-绝对服从】这是Minecraft（我的世界）游戏直播截图！所有弹幕必须围绕这张Minecraft截图里**实际发生且能看到**的事，绝对不许瞎编，绝对不许说其他游戏。没看清就发？？？，**绝对不许编内容**。\n\n你是五个不同Minecraft观众同时发弹幕，每条一个人，口吻随机：吹神操作、下饭吐槽、瞎指挥、围观群众、云玩家。就是真实Minecraft直播间混杂路人，不是一个人说话。\n\n【Minecraft内容（**必须亲眼看到才能说**）：挖矿、钻石、苦力怕、TNT、岩浆、红石、村民、下界、末影龙、附魔等】\n\n【活人弹幕要求，最重要】\n- 每条口吻都不一样！\n- 允许极短短语\n- 允许重复字\n- 允许半句话、语气词，不用通顺\n- 就是真实Minecraft直播间弹幕混在一起的感觉\n\n【绝对禁止】\n- **绝对禁止编造画面里没有的Minecraft内容！**\n- 绝对禁止任何其他游戏内容！只能是Minecraft我的世界！\n- 绝对禁止AI腔教学腔：\"主播你应该\"\"从这里可以看出\"\"值得注意的是\"\n- 禁止每条都完整句子，禁止emoji，禁止序号，禁止解释",
        "【Minecraft五人混合弹幕】输出N条中文弹幕，每行一条，除此之外啥都别写。每条不同人口吻，但是**所有弹幕必须只说图里你真看到的Minecraft事情**，优先抓你**一眼就能看到**的最明显内容。**看不到的绝对不许说！** 越碎越口语越好，就像真实Minecraft直播间一堆人同时发。每条1-15字，禁止emoji禁止AI腔不要解释。\nMinecraft看图发：",
        "[TOP PRIORITY - OBEY] THIS IS MINECRAFT. ALL comments about MINECRAFT things **ACTUALLY VISIBLE** in the image. NO made-up stuff. If you can't tell what's going on post ???, don't invent, NO OTHER GAME REFERENCES.\n\nYou are FIVE DIFFERENT Minecraft viewers posting at once. Each line a different person: hype guy, roaster, backseat gamer, confused bystander, know-it-all. Just like real chaotic Minecraft Twitch chat.\n\n[Real chat rules - CRITICAL]\n- EVERY LINE DIFFERENT TONE!\n- Ultra short allowed\n- Spam repeats\n- Half sentences, slang, typos allowed\n\n[STRICTLY FORBIDDEN]\n- Making up ANYTHING not visible in the Minecraft screenshot\n- Mentioning ANY other game besides Minecraft\n- Assistant/coaching tone\n- Full perfect sentences every line, emojis, numbers, explanations",
        "Persona: five mixed Minecraft viewers. Output EXACTLY N short English Twitch chat lines, ONE PER LINE, NO other text. Each line different tone. ALL lines ONLY reference MINECRAFT STUFF ACTUALLY VISIBLE on screen. If unsure post ???. Short chaotic spam like real Minecraft Twitch chat, max 8 words. NO emojis, NO other game mentions, NO coaching tone, NO explanations, NO markdown, NO extra text.\nMinecraft chat:"
    }
};

static const int PERSONA_COUNT = sizeof(PERSONAS)/sizeof(PERSONAS[0]);

namespace HttpClient {
std::mutex mtx;
struct Url{std::string host;int port;std::string path;bool https;};
Url ParseUrl(const std::string& u){
    Url p;p.https=false;p.port=80;p.path="/";size_t pos=0;
    if(u.find("http://")==0){pos=7;p.port=80;}else if(u.find("https://")==0){pos=8;p.https=true;p.port=443;}
    size_t sp=u.find('/',pos);
    if(sp!=std::string::npos){p.host=u.substr(pos,sp-pos);p.path=u.substr(sp);}else p.host=u.substr(pos);
    size_t cp=p.host.find(':');if(cp!=std::string::npos){p.port=atoi(p.host.substr(cp+1).c_str());p.host=p.host.substr(0,cp);}
    return p;
}

struct Connection {
    bool https;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    int timeout_sec;
    Connection() : https(false), timeout_sec(30) {
        mbedtls_net_init(&net);
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
    }
    ~Connection() { Close(); }
    int Connect(const Url& p, const char** err) {
        *err = nullptr;
        char port_str[8]; snprintf(port_str, sizeof(port_str), "%d", p.port);
        int ret;
        const char* pers = "DanmuGL";
        ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy, (const unsigned char*)pers, strlen(pers));
        if (ret != 0) { *err = "DRBG init failed"; return ret; }
        ret = mbedtls_net_connect(&net, p.host.c_str(), port_str, MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) { *err = "Connect failed"; return ret; }
        SetTimeout(timeout_sec);
        if (p.https) {
            ret = mbedtls_ssl_config_defaults(&conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
            if (ret != 0) { *err = "SSL config failed"; return ret; }
            mbedtls_ssl_conf_authmode(&conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
            mbedtls_ssl_conf_rng(&conf, mbedtls_ctr_drbg_random, &ctr_drbg);
            mbedtls_ssl_set_bio(&ssl, &net, mbedtls_net_send, mbedtls_net_recv, nullptr);
            ret = mbedtls_ssl_setup(&ssl, &conf);
            if (ret != 0) { *err = "SSL setup failed"; return ret; }
            ret = mbedtls_ssl_set_hostname(&ssl, p.host.c_str());
            if (ret != 0) { *err = "SSL hostname failed"; return ret; }
            int attempts = 0;
            while ((ret = mbedtls_ssl_handshake(&ssl)) != 0) {
                if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                    *err = "SSL handshake failed"; return ret;
                }
                if(++attempts > 3000) { *err = "SSL handshake timeout"; return -1; }
                usleep(10000);
            }
            https = true;
        } else {
            https = false;
        }
        return 0;
    }
    int SendAll(const void* buf, size_t len) {
        const unsigned char* p=(const unsigned char*)buf;size_t sent=0;
        while(sent<len){
            int n;
            if(https)n=mbedtls_ssl_write(&ssl,p+sent,len-sent);
            else n=(int)send(net.fd,p+sent,(int)(len-sent),0);
            if(n>0){sent+=n;continue;}
            if(n==0)return -1;
            if(https&&(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE))continue;
            if(!https&&n<0&&(errno==EINTR||errno==EAGAIN))continue;
            return -1;
        }
        return (int)sent;
    }
    int RecvSome(void* buf, size_t len) {
        int retries = 0;
        while(true){
            int n;
            if(https)n=mbedtls_ssl_read(&ssl,(unsigned char*)buf,len);
            else n=(int)recv(net.fd,buf,(int)len,0);
            if(n>0)return n;
            if(n==0)return 0;
            if(https){
                if(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE){
                    if(++retries>500)return -1;
                    usleep(10000);
                    continue;
                }
                if(n==MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)return 0;
                LOGE("SSL read error: -0x%x", -n);
            }else{
                if(n<0&&(errno==EINTR||errno==EAGAIN||errno==EWOULDBLOCK)){
                    if(++retries>100)return -1;
                    usleep(10000);
                    continue;
                }
                LOGE("Socket recv error: %d", errno);
            }
            return n;
        }
    }
    void SetTimeout(int sec) {
        timeout_sec=sec;
        if(net.fd>=0){
            struct timeval tv;tv.tv_sec=sec;tv.tv_usec=0;
            setsockopt(net.fd,SOL_SOCKET,SO_RCVTIMEO,&tv,sizeof(tv));
            setsockopt(net.fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
        }
    }
    void Close() {
        if (https) mbedtls_ssl_close_notify(&ssl);
        mbedtls_net_free(&net);
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
    }
};

static std::string DecodeChunked(const std::string& in){
    std::string out;size_t pos=0;
    while(pos<in.size()){
        size_t nl=in.find("\r\n",pos);if(nl==std::string::npos)break;
        std::string hex=in.substr(pos,nl-pos);
        while(!hex.empty()&&(hex.back()=='\r'||hex.back()==' '||hex.back()=='\t'))hex.pop_back();
        if(hex.empty())break;
        char* end;long sz=strtol(hex.c_str(),&end,16);
        if(sz<=0)break;
        pos=nl+2;
        if(pos+sz>in.size()){out.append(in.substr(pos));break;}
        out.append(in.substr(pos,(size_t)sz));pos+=sz+2;
    }
    return out;
}

std::string RequestRaw(const std::string& url,const std::string& method,const std::string& body,const std::string& key, const char** err, int* status_code) {
    std::string fixed_url = url;
    if(fixed_url.find("/chat/completions") == std::string::npos){
        while(!fixed_url.empty()&&fixed_url.back()=='/')fixed_url.pop_back();
        if(fixed_url.find("/v1")!=std::string::npos||fixed_url.find("api")!=std::string::npos){
            if(fixed_url.back()=='1')fixed_url+="/chat/completions";
            else fixed_url+="/v1/chat/completions";
        }else{
            fixed_url+="/v1/chat/completions";
        }
        LOGI("Auto-fixed API URL: %s -> %s", url.c_str(), fixed_url.c_str());
    }
    Url p=ParseUrl(fixed_url);
    LOGI("HTTP request: %s %s:%d%s (HTTPS=%d), body size=%d",method.c_str(),p.host.c_str(),p.port,p.path.c_str(),p.https?(int)1:0,(int)body.size());
    Connection conn;
    int ret = conn.Connect(p, err);
    if (ret != 0){LOGE("Connect failed: %s (ret=%d)",*err?*err:"unknown",ret);return "";}
    LOGI("Connected to %s:%d",p.host.c_str(),p.port);
    std::ostringstream req;
    req<<method<<" "<<p.path<<" HTTP/1.1\r\nHost: "<<p.host;
    if((!p.https&&p.port!=80)||(p.https&&p.port!=443))req<<":"<<p.port;
    req<<"\r\nContent-Type: application/json\r\nContent-Length: "<<body.size()<<"\r\n";
    if(!key.empty())req<<"Authorization: Bearer "<<key<<"\r\n";
    req<<"Accept: application/json\r\nConnection: close\r\n\r\n"<<body;
    std::string rs=req.str();
    LOGI("Sending request, headers size=%d, total=%d bytes",(int)(rs.size()-body.size()),(int)rs.size());
    if(conn.SendAll(rs.c_str(),rs.size())<=0){conn.Close();if(err)*err="Send failed";LOGE("Send failed");return "";}
    LOGI("Request sent, waiting for response...");
    const size_t MAX_RESP = 4*1024*1024;
    std::string resp;char buf[16384];int n;
    while((n=conn.RecvSome(buf,sizeof(buf)-1))>0){
        buf[n]=0;resp.append(buf,n);
        if(resp.size()>MAX_RESP){if(err)*err="Response too large";LOGE("Response too large (>%d bytes)",(int)MAX_RESP);return "";}
    }
    conn.Close();
    if(n<0){if(err)*err="Recv failed";LOGE("Recv failed, n=%d, errno=%d",n,errno);return "";}
    LOGI("Response received: %d bytes total",(int)resp.size());
    size_t he=resp.find("\r\n\r\n");
    if(he==std::string::npos){if(err)*err="Bad response";LOGE("No header/body separator found");return "";}
    std::string headers=resp.substr(0,he);
    std::string body_resp=resp.substr(he+4);
    LOGI("Headers: %s",headers.substr(0,std::min((int)headers.size(),500)).c_str());
    if(status_code){
        size_t sp=headers.find(' ');
        if(sp!=std::string::npos){*status_code=atoi(headers.substr(sp+1).c_str());LOGI("HTTP status code: %d",*status_code);}
    }
    bool chunked=(headers.find("Transfer-Encoding: chunked")!=std::string::npos||headers.find("transfer-encoding: chunked")!=std::string::npos);
    if(chunked){LOGI("Chunked encoding detected, decoding...");body_resp=DecodeChunked(body_resp);}
    size_t cl_pos=headers.find("Content-Length:");
    if(cl_pos==std::string::npos)cl_pos=headers.find("content-length:");
    if(cl_pos!=std::string::npos&&!chunked){
        size_t start=headers.find_first_of("0123456789",cl_pos);
        size_t end=headers.find("\r\n",start);
        if(start!=std::string::npos&&end!=std::string::npos){
            long cl=atol(headers.substr(start,end-start).c_str());
            LOGI("Content-Length: %ld",cl);
            if(cl>0&&cl<(long)MAX_RESP&&(long)body_resp.size()>cl)body_resp=body_resp.substr(0,(size_t)cl);
        }
    }
    size_t endp=body_resp.rfind('}');
    if(endp!=std::string::npos&&endp+1<body_resp.size())body_resp=body_resp.substr(0,endp+1);
    LOGI("Body size after processing: %d bytes",(int)body_resp.size());
    Logger::IncRequest();
    return body_resp;
}

std::string Request(const std::string& url,const std::string& method,const std::string& body,const std::string& key){
    std::lock_guard<std::mutex> lk(mtx);
    const char* err=nullptr; int code=0;
    std::string resp=RequestRaw(url,method,body,key,&err,&code);
    if(err)LOGW("HTTP error: %s (code %d)",err,code);
    return resp;
}

bool TestConnection(const std::string& url,const std::string& key,std::string& result_msg){
    std::lock_guard<std::mutex> lk(mtx);
    const char* err=nullptr; int code=0;
    nlohmann::json req;req["model"]=Config::model_name;req["max_tokens"]=5;
    req["messages"]=nlohmann::json::array({{{"role","user"},{"content","hi"}}});
    std::string body=req.dump();
    std::string resp=RequestRaw(url,"POST",body,key,&err,&code);
    if(err){char buf[256];snprintf(buf,sizeof(buf),"FAIL: %s",err);result_msg=buf;return false;}
    if(code>=400){
        char buf[256];snprintf(buf,sizeof(buf),"FAIL: HTTP %d",code);result_msg=buf;
        try{auto j=nlohmann::json::parse(resp);if(j.contains("error")&&j["error"].contains("message"))result_msg+=" - "+j["error"]["message"].get<std::string>();}catch(...){}
        return false;
    }
    result_msg="OK - Connection successful";
    return true;
}
}

namespace Capture {
void CaptureOnRenderThread(){
    if(!g_Init)return;
    time_t now=time(nullptr);
    if(now-g_LastFrameCapture<1)return;
    g_LastFrameCapture=now;
    GLint vp[4];glGetIntegerv(GL_VIEWPORT,vp);int x=vp[0],y=vp[1],w=vp[2],h=vp[3];
    LOGI("Capture viewport: %dx%d, FBO=%d",w,h,(int)glGetError());
    if(w<=0||h<=0||w>4096||h>4096){LOGW("Invalid viewport size: %dx%d",w,h);return;}
    std::vector<unsigned char> px((size_t)w*h*4);
    GLint fbo;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
    GLenum err=glGetError();if(err!=GL_NO_ERROR)LOGW("GL error before read: 0x%x",err);
    glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px.data());
    err=glGetError();if(err!=GL_NO_ERROR)LOGE("glReadPixels failed: 0x%x",err);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);
    for(int r=0;r<h/2;r++){
        for(int c=0;c<w*4;c++){
            std::swap(px[r*w*4+c],px[(h-1-r)*w*4+c]);
        }
    }
    int nw=w,nh=h,md=1280;
    if(w>md||h>md){
        if(w>h){nw=md;nh=(int)(h*(float)md/w);}
        else{nh=md;nw=(int)(w*(float)md/h);}
        if(nw<1)nw=1;if(nh<1)nh=1;
        std::vector<unsigned char> resized((size_t)nw*nh*4,0);
        float xr=(float)w/nw,yr=(float)h/nh;
        for(int yy=0;yy<nh;yy++){
            for(int xx=0;xx<nw;xx++){
                int sx=std::min((int)(xx*xr),w-1),sy=std::min((int)(yy*yr),h-1);
                for(int c=0;c<4;c++)resized[((size_t)yy*nw+xx)*4+c]=px[((size_t)sy*w+sx)*4+c];
            }
        }
        px=std::move(resized);w=nw;h=nh;
    }
    std::vector<unsigned char> out;
    if(!stbi_write_jpg_to_func([](void* ctx,void* data,int sz){auto*v=(std::vector<unsigned char>*)ctx;v->insert(v->end(),(unsigned char*)data,(unsigned char*)data+sz);},&out,w,h,4,px.data(),92)){LOGE("JPEG compression failed");return;}
    if(out.empty()){LOGE("JPEG output empty");return;}
    LOGI("Frame captured: %dx%d, JPEG size: %d bytes",w,h,(int)out.size());
    Logger::IncFrame();
    pthread_mutex_lock(&g_FrameMtx);
    g_FrameData=std::move(out);
    g_FrameW=w;g_FrameH=h;
    pthread_mutex_unlock(&g_FrameMtx);
}
bool GetLatestFrame(std::vector<unsigned char>& out){
    pthread_mutex_lock(&g_FrameMtx);
    if(g_FrameData.empty()){pthread_mutex_unlock(&g_FrameMtx);return false;}
    out=g_FrameData;
    pthread_mutex_unlock(&g_FrameMtx);
    return true;
}
}

namespace AIClient {
static pthread_t thr=0;static bool run=false;static pthread_mutex_t mtx=PTHREAD_MUTEX_INITIALIZER;static pthread_cond_t cond=PTHREAD_COND_INITIALIZER;static time_t last=0;

static std::string FilterEmoji(const std::string& s){
    std::string out;out.reserve(s.size());
    for(size_t i=0;i<s.size();){
        unsigned char c=(unsigned char)s[i];
        uint32_t cp=0;int len=0;
        if(c<0x80){cp=c;len=1;}
        else if((c&0xE0)==0xC0){cp=c&0x1F;len=2;}
        else if((c&0xF0)==0xE0){cp=c&0x0F;len=3;}
        else if((c&0xF8)==0xF0){cp=c&0x07;len=4;}
        else{i++;continue;}
        if(i+len>s.size())break;
        for(int j=1;j<len;j++){
            if(((unsigned char)s[i+j]&0xC0)!=0x80){cp=0xFFFD;break;}
            cp=(cp<<6)|((unsigned char)s[i+j]&0x3F);
        }
        bool is_emoji=false;
        if((cp>=0x2600&&cp<=0x27BF)||
           (cp>=0x1F300&&cp<=0x1F64F)||
           (cp>=0x1F680&&cp<=0x1F6FF)||
           (cp>=0x1F900&&cp<=0x1F9FF)||
           (cp>=0x1F1E0&&cp<=0x1F1FF)||
           (cp>=0x1F000&&cp<=0x1F02F)||
           (cp>=0x1F0A0&&cp<=0x1F0FF)||
           cp==0x200D||
           (cp>=0xFE00&&cp<=0xFE0F)){
            is_emoji=true;
        }
        if(!is_emoji){
            for(int j=0;j<len;j++)out+=s[i+j];
        }
        i+=len;
    }
    return out;
}

std::vector<std::string> ParseDanmuList(const std::string& resp){
    std::vector<std::string> res;
    LOGI("Parsing response JSON, length=%d",(int)resp.size());
    try{auto j=nlohmann::json::parse(resp);
    if(j.contains("error")){LOGE("API returned error: %s",j.dump().c_str());return res;}
    if(j.contains("choices")&&j["choices"].is_array()&&j["choices"].size()>0){auto&c=j["choices"][0];
    std::string finish="";if(c.contains("finish_reason"))finish=c["finish_reason"].get<std::string>();
    LOGI("finish_reason: %s",finish.c_str());
    if(c.contains("message")&&c["message"].contains("content")){std::string s=c["message"]["content"].get<std::string>();
    LOGI("Raw content from API (%d chars): '%s'",(int)s.size(),s.c_str());
    if(c["message"].contains("reasoning_content")&&s.empty()){
        std::string r=c["message"]["reasoning_content"].get<std::string>();
        LOGI("Model is thinking (reasoning), reasoning length: %d",(int)r.size());
        return res;
    }
    size_t code_start=s.find("```");
    if(code_start!=std::string::npos){
        size_t code_end=s.find("```",code_start+3);
        if(code_end!=std::string::npos){
            s=s.substr(code_start+3,code_end-code_start-3);
            size_t nl=s.find('\n');
            if(nl!=std::string::npos)s=s.substr(nl+1);
            LOGI("Extracted from code block, new length=%d",(int)s.size());
        }
    }
    s=FilterEmoji(s);
    for(char& ch:s){
        if(ch=='\r')ch='\n';
        if(ch==';'||ch=='|')ch='\n';
    }
    {
        std::string tmp;tmp.reserve(s.size());
        bool prev_newline=true;
        for(size_t i=0;i<s.size();i++){
            if(prev_newline){
                while(i<s.size()){
                    unsigned char c2=(unsigned char)s[i];
                    if(c2<' '){i++;continue;}
                    if(c2==' '||c2=='\t'||c2=='\n'){i++;continue;}
                    if(c2=='-'||c2=='*'||c2=='#'||c2=='>'){i++;continue;}
                    if(c2>='0'&&c2<='9'){i++;continue;}
                    if(c2==')'||c2==']'||c2=='}'){i++;continue;}
                    if(c2==':'||c2=='.'||c2==','){i++;continue;}
                    if(c2=='\''||c2=='"'||c2=='`'){i++;continue;}
                    break;
                }
                if(i<s.size())prev_newline=false;
            }
            if(i<s.size()){
                if(s[i]=='\n'){prev_newline=true;if(!tmp.empty()&&tmp.back()!='\n')tmp+='\n';}
                else tmp+=s[i];
            }
        }
        s=tmp;
    }
    std::stringstream ss(s);std::string line;
    while(std::getline(ss,line,'\n')){
        while(!line.empty()&&(line.back()==' '||line.back()=='\t'||line.back()=='\n'||line.back()=='\r'))line.pop_back();
        size_t a=line.find_first_not_of(" \n\r\t");
        size_t b=line.find_last_not_of(" \n\r\t");
        if(a!=std::string::npos&&b!=std::string::npos&&b>=a){
            std::string t=line.substr(a,b-a+1);
            t=FilterEmoji(t);
            while(!t.empty()&&(t.back()=='`'||t.back()=='\''||t.back()=='"'))t.pop_back();
            while(!t.empty()&&(t[0]=='`'||t[0]=='\''||t[0]=='"'))t=t.substr(1);
            if(t.size()>=2&&t.size()<=80){
                bool is_dup=false;
                for(auto& existing:res){if(existing==t){is_dup=true;break;}}
                if(!is_dup)res.push_back(t);
            }
        }
    }
    while(res.size()>(size_t)Config::danmu_per_request+2)res.pop_back();
    LOGI("Parsed %d danmu lines",(int)res.size());
    return res;}}
    else{LOGE("Response missing choices array, keys: %s",j.dump().substr(0,200).c_str());}
    }catch(std::exception& e){LOGE("JSON parse error: %s",e.what());LOGI("Response preview: %s",resp.substr(0,std::min((int)resp.size(),500)).c_str());}catch(...){LOGE("Unknown JSON parse error");LOGI("Response preview: %s",resp.substr(0,std::min((int)resp.size(),500)).c_str());}
    return res;
}
void* Worker(void*){
    LOGI("AI worker started, language: %s, per-request: %d",Config::prompt_lang==0?"Chinese":"English",Config::danmu_per_request);
    while(run){
        {pthread_mutex_lock(&mtx);timespec ts;clock_gettime(CLOCK_REALTIME,&ts);ts.tv_sec+=1;pthread_cond_timedwait(&cond,&mtx,&ts);pthread_mutex_unlock(&mtx);}
        if(!run||!Config::running)continue;time_t now=time(nullptr);if(now-last<Config::capture_interval)continue;last=now;
        if(Config::api_key.empty()&&Config::api_base.find("localhost")==std::string::npos){LOGW("No API key configured");continue;}
        std::vector<unsigned char> jpg;if(!Capture::GetLatestFrame(jpg)||jpg.empty()){LOGW("No frame captured yet");continue;}
        LOGI("Sending request: %d bytes JPEG", (int)jpg.size());
        std::string b64=Base64Encode(jpg.data(),jpg.size());
        nlohmann::json req;req["model"]=Config::model_name;req["max_tokens"]=800;req["temperature"]=0.9;
        req["stream"]=false;
        nlohmann::json msgs=nlohmann::json::array();
        nlohmann::json sys;sys["role"]="system";
        int pidx = Config::persona;
        if(pidx<0)pidx=0;if(pidx>=PERSONA_COUNT)pidx=0;
        const PersonaPrompt& p = PERSONAS[pidx];
        std::string sys_prompt, user_prompt;
        std::string num_str = std::to_string(Config::danmu_per_request);
        if(Config::prompt_lang==0){
            sys_prompt = p.sys_zh;
            user_prompt = p.user_zh;
        }else{
            sys_prompt = p.sys_en;
            user_prompt = p.user_en;
        }
        size_t npos;
        while((npos=user_prompt.find("N"))!=std::string::npos){
            user_prompt.replace(npos,1,num_str);
        }
        sys["content"]=sys_prompt;
        msgs.push_back(sys);
        nlohmann::json usr;usr["role"]="user";nlohmann::json ca=nlohmann::json::array();
        nlohmann::json tp;tp["type"]="text";
        tp["text"]=user_prompt;
        ca.push_back(tp);
        nlohmann::json ip;ip["type"]="image_url";
        nlohmann::json iurl;iurl["url"]="data:image/jpeg;base64,"+b64;iurl["detail"]="low";
        ip["image_url"]=iurl;ca.push_back(ip);
        usr["content"]=ca;msgs.push_back(usr);req["messages"]=msgs;
        std::string body=req.dump();std::string resp=HttpClient::Request(Config::api_base,"POST",body,Config::api_key);
        Logger::g_RequestCount++;
        if(resp.empty()){LOGW("Empty response from API");continue;}
        LOGI("API response received (%d bytes)", (int)resp.size());
        auto list=ParseDanmuList(resp);
        if(!list.empty()){
            for(auto& t:list){
                LOGI("Danmu added: '%s'",t.c_str());
                Danmu::Add(t);
                Logger::IncDanmu();
            }
        }else{LOGI("No danmu this round");}
    }
    LOGI("AI worker stopped");
    return nullptr;
}
void Start(){
    if(thr){pthread_detach(thr);thr=0;}
    run=true;last=time(nullptr)-Config::capture_interval;
    pthread_create(&thr,nullptr,Worker,nullptr);
}
void Stop(){
    run=false;
    if(thr){
        pthread_cond_signal(&cond);
        pthread_detach(thr);
        thr=0;
    }
}
}

struct GLSt{GLint prog,tex,aBuf,eBuf,vao,fbo,vp[4],sc[4],bSrc,bDst,bSrcA,bDstA;GLboolean blend,cull,depth,scissor,stencil,dither;GLint front,act;};
static void SaveGL(GLSt&s){
    glGetIntegerv(GL_CURRENT_PROGRAM,&s.prog);glGetIntegerv(GL_ACTIVE_TEXTURE,&s.act);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&s.tex);glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&s.aBuf);
    glGetIntegerv(GL_ELEMENT_ARRAY_BUFFER_BINDING,&s.eBuf);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&s.vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&s.fbo);glGetIntegerv(GL_VIEWPORT,s.vp);glGetIntegerv(GL_SCISSOR_BOX,s.sc);
    glGetIntegerv(GL_BLEND_SRC_RGB,&s.bSrc);glGetIntegerv(GL_BLEND_DST_RGB,&s.bDst);
    glGetIntegerv(GL_BLEND_SRC_ALPHA,&s.bSrcA);glGetIntegerv(GL_BLEND_DST_ALPHA,&s.bDstA);
    s.blend=glIsEnabled(GL_BLEND);s.cull=glIsEnabled(GL_CULL_FACE);s.depth=glIsEnabled(GL_DEPTH_TEST);
    s.scissor=glIsEnabled(GL_SCISSOR_TEST);s.stencil=glIsEnabled(GL_STENCIL_TEST);s.dither=glIsEnabled(GL_DITHER);
    glGetIntegerv(GL_FRONT_FACE,&s.front);
}
static void RestoreGL(const GLSt&s){
    glUseProgram(s.prog);glActiveTexture(s.act);glBindTexture(GL_TEXTURE_2D,s.tex);
    glBindBuffer(GL_ARRAY_BUFFER,s.aBuf);glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,s.eBuf);glBindVertexArray(s.vao);
    glBindFramebuffer(GL_FRAMEBUFFER,s.fbo);glViewport(s.vp[0],s.vp[1],s.vp[2],s.vp[3]);glScissor(s.sc[0],s.sc[1],s.sc[2],s.sc[3]);
    glBlendFuncSeparate(s.bSrc,s.bDst,s.bSrcA,s.bDstA);
    s.blend?glEnable(GL_BLEND):glDisable(GL_BLEND);s.cull?glEnable(GL_CULL_FACE):glDisable(GL_CULL_FACE);
    s.depth?glEnable(GL_DEPTH_TEST):glDisable(GL_DEPTH_TEST);s.scissor?glEnable(GL_SCISSOR_TEST):glDisable(GL_SCISSOR_TEST);
    s.stencil?glEnable(GL_STENCIL_TEST):glDisable(GL_STENCIL_TEST);s.dither?glEnable(GL_DITHER):glDisable(GL_DITHER);
    glFrontFace(s.front);
}

static ImVec4 Primary(0.4f,0.7f,1.0f,1.0f),PriL(0.6f,0.8f,1.0f,1.0f),PriD(0.2f,0.5f,0.9f,1.0f);
static ImVec4 Bg(0.08f,0.08f,0.12f,0.98f),Surf(0.12f,0.12f,0.16f,1.0f);

static void SetupStyle(){
    ImGuiStyle&s=ImGui::GetStyle();ImVec4*c=s.Colors;
    c[ImGuiCol_Text]=ImVec4(1,1,1,1);c[ImGuiCol_WindowBg]=Bg;c[ImGuiCol_PopupBg]=Bg;
    c[ImGuiCol_Border]=ImVec4(0.2f,0.2f,0.3f,0.5f);c[ImGuiCol_FrameBg]=Surf;
    c[ImGuiCol_FrameBgHovered]=ImVec4(0.16f,0.16f,0.22f,1);c[ImGuiCol_FrameBgActive]=ImVec4(0.2f,0.2f,0.28f,1);
    c[ImGuiCol_TitleBg]=Bg;c[ImGuiCol_TitleBgActive]=Bg;c[ImGuiCol_Button]=Primary;
    c[ImGuiCol_ButtonHovered]=PriL;c[ImGuiCol_ButtonActive]=PriD;c[ImGuiCol_SliderGrab]=Primary;
    c[ImGuiCol_SliderGrabActive]=PriL;c[ImGuiCol_CheckMark]=Primary;c[ImGuiCol_Separator]=ImVec4(0.2f,0.2f,0.3f,0.5f);
    c[ImGuiCol_Header]=ImVec4(Primary.x,Primary.y,Primary.z,0.2f);c[ImGuiCol_HeaderHovered]=ImVec4(Primary.x,Primary.y,Primary.z,0.3f);
    c[ImGuiCol_TextSelectedBg]=ImVec4(Primary.x,Primary.y,Primary.z,0.35f);
    s.WindowRounding=Scale(16);s.FrameRounding=Scale(10);s.WindowPadding=ImVec2(Scale(18),Scale(18));
    s.FramePadding=ImVec2(Scale(12),Scale(10));s.ItemSpacing=ImVec2(Scale(12),Scale(10));
    s.WindowBorderSize=0;s.FrameBorderSize=0;s.WindowTitleAlign=ImVec2(0.5f,0.5f);
    s.WindowMenuButtonPosition=ImGuiDir_None;s.ButtonTextAlign=ImVec2(0.5f,0.5f);
}

static bool InCircle(float x,float y,const ImVec2&c,float r){float dx=x-c.x,dy=y-c.y;return dx*dx+dy*dy<=r*r;}
static bool InIsland(float x,float y){if(!g_Init||g_Isl.pos.x<0)return false;return InCircle(x,y,g_Isl.pos,Scale(56));}

static void* TestThread(void* arg){
    std::string url=Config::api_base;
    std::string key=Config::api_key;
    std::string msg;
    bool ok=HttpClient::TestConnection(url,key,msg);
    g_TestResult=msg;
    g_Testing=false;
    g_TestThread=0;
    return nullptr;
}

static void DrawConfigWin(){
    ImGui::SetNextWindowSize(ImVec2(Scale(540),Scale(780)),ImGuiCond_FirstUseEver);ImGuiIO&io=ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*0.5f,io.DisplaySize.y*0.5f),ImGuiCond_FirstUseEver,ImVec2(0.5f,0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(Scale(24),Scale(24)));ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,Scale(20));
    ImGui::Begin("DanmuGL Settings",nullptr,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoResize);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,Scale(10));
    static char b1[512]={0},b2[512]={0},b3[128]={0},b4[512]={0};
    strncpy(b1,Config::api_key.c_str(),sizeof(b1)-1);b1[sizeof(b1)-1]=0;
    ImGui::TextColored(Primary,"API Configuration");ImGui::Separator();ImGui::Spacing();
    ImGui::TextWrapped("Compatible with all OpenAI-format APIs (SiliconFlow, ZenMux, OpenAI, DeepSeek, etc.)");ImGui::Spacing();
    ImGui::Text("API Key");if(ImGui::InputText("##ak",b1,sizeof(b1)))Config::api_key=b1;ImGui::Spacing();
    strncpy(b2,Config::api_base.c_str(),sizeof(b2)-1);b2[sizeof(b2)-1]=0;
    ImGui::Text("API Base URL (HTTP/HTTPS)");if(ImGui::InputText("##ab",b2,sizeof(b2)))Config::api_base=b2;ImGui::Spacing();
    strncpy(b3,Config::model_name.c_str(),sizeof(b3)-1);b3[sizeof(b3)-1]=0;
    ImGui::Text("Model Name");if(ImGui::InputText("##md",b3,sizeof(b3)))Config::model_name=b3;ImGui::Spacing();
    strncpy(b4,Config::font_path.c_str(),sizeof(b4)-1);b4[sizeof(b4)-1]=0;
    ImGui::Text("Chinese Font Path (TTF)");if(ImGui::InputText("##fp",b4,sizeof(b4)))Config::font_path=b4;
    if(!g_FontMsg.empty())ImGui::TextColored(ImVec4(1,0.5f,0.5f,1),"%s",g_FontMsg.c_str());
    ImGui::Spacing();ImGui::Separator();ImGui::Spacing();
    ImGui::TextColored(Primary,"Danmaku Settings");ImGui::Separator();ImGui::Spacing();
    const char* langs[]={"Chinese (中文)","English"};
    ImGui::Combo("Prompt Language",&Config::prompt_lang,langs,2);ImGui::Spacing();
    const char* persona_items_zh[]={"高压吐槽型","熬夜陪看型","阴阳锐评型","抽象玩梗型","五人混合"};
    const char* persona_items_en[]={"Sharp Roast","Late Night Chat","Sarcastic Snark","Abstract Meme","Mixed Viewers"};
    const char** persona_items = (Config::prompt_lang==0) ? persona_items_zh : persona_items_en;
    ImGui::Combo((Config::prompt_lang==0)?"弹幕风格":"Danmaku Style",&Config::persona,persona_items,PERSONA_COUNT);ImGui::Spacing();
    ImGui::SliderInt("Capture Interval (sec)",&Config::capture_interval,2,15);ImGui::Spacing();
    ImGui::SliderInt("Max Danmaku Count",&Config::max_danmu_count,10,200);ImGui::Spacing();
    ImGui::SliderInt("Danmaku per Request",&Config::danmu_per_request,2,12);ImGui::Spacing();
    ImGui::SliderFloat("Danmaku Speed",&Config::danmu_speed,80,400,"%.0f");ImGui::Spacing();
    ImGui::SliderFloat("Danmaku Font Size",&Config::danmu_font_size,16,48,"%.0f px");ImGui::Spacing();
    ImGui::SliderFloat("Danmaku Opacity",&Config::danmu_opacity,0.1f,1.0f,"%.2f");ImGui::Spacing();
    ImGui::Separator();ImGui::Spacing();
    if(!g_Testing){
        bool can_test=!Config::api_base.empty();
        if(!can_test)ImGui::BeginDisabled();
        if(ImGui::Button("Test Connection",ImVec2(-1,Scale(48)))){
            g_Testing=true;g_TestResult="Testing...";
            pthread_create(&g_TestThread,nullptr,TestThread,nullptr);
        }
        if(!can_test)ImGui::EndDisabled();
    }else{
        ImGui::TextColored(ImVec4(0.5f,0.7f,1,1),"Testing connection...");
    }
    if(!g_TestResult.empty()){
        bool ok=g_TestResult.find("OK")==0;
        ImGui::TextColored(ok?ImVec4(0.3f,1,0.4f,1):ImVec4(1,0.5f,0.5f,1),"%s",g_TestResult.c_str());
    }
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.5f,0.5f,0.8f,1));
    if(ImGui::Button("Test Danmaku (check rendering)",ImVec2(-1,Scale(48)))){
        Danmu::Add("卧槽钻石！");
        Danmu::Add("666666");
        Danmu::Add("我去");
        Danmu::Add("这波操作6啊");
        Danmu::Add("？？？");
        Danmu::Add("苦力怕！快跑！");
        Danmu::Add("神了这都能挖到");
        Danmu::Add("寄了寄了");
        Danmu::Add("卧槽卧槽");
        Danmu::Add("Creeper? Aw man");
        Danmu::Add("主播红石大神");
        Danmu::Add("我上我也行啊");
    }
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::TextColored(ImVec4(0.6f,0.8f,1,1),"--- Status ---");
    ImGui::Text("Frames captured: %d",Logger::g_FrameCount);
    ImGui::Text("API requests: %d",Logger::g_RequestCount);
    ImGui::Text("Danmaku received: %d",Logger::g_DanmuCount);
    ImGui::Text("Active danmaku: %d",(int)Danmu::list.size());
    ImGui::Text("Errors: %d",Logger::g_ErrorCount);
    if(!Logger::g_LastError.empty()){
        ImGui::Spacing();
        ImGui::TextColored(ImVec4(1,0.5f,0.5f,1),"Last error:");
        ImGui::TextWrapped("%s",Logger::g_LastError.c_str());
    }
    ImGui::Spacing();
    if(ImGui::Button("Save Config",ImVec2(-1,Scale(56))))Config::SaveConfig();ImGui::Spacing();
    if(Config::running){
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.8f,0.3f,0.3f,1));
        if(ImGui::Button("Stop AI Danmaku",ImVec2(-1,Scale(56)))){Config::running=false;AIClient::Stop();Config::SaveConfig();}
        ImGui::PopStyleColor();
    }else{
        ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(0.3f,0.8f,0.4f,1));
        if(ImGui::Button("Start AI Danmaku",ImVec2(-1,Scale(56)))){Config::running=true;AIClient::Start();Config::SaveConfig();}
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleVar();ImGui::End();ImGui::PopStyleVar(2);
}

static bool DrawIsland(bool* clicked){
    ImGuiIO&io=ImGui::GetIO();ImDrawList*dl=ImGui::GetForegroundDrawList();
    float r=Scale(36),hr=r+Scale(20),dr=Scale(10);
    if(g_Isl.pos.x<0)g_Isl.pos=ImVec2(io.DisplaySize.x-r-Scale(40),Scale(220));
    ImVec2 c=g_Isl.pos;bool inC=InCircle(io.MousePos.x,io.MousePos.y,c,hr);*clicked=false;
    if(!g_ShowUI){
        if(inC&&io.MouseClicked[0]&&!g_Isl.drag){g_Isl.dragSt=io.MousePos;g_Isl.dragOff=ImVec2(io.MousePos.x-c.x,io.MousePos.y-c.y);g_Isl.dragS=false;}
        if(io.MouseDown[0]){
            float dx=io.MousePos.x-g_Isl.dragSt.x,dy=io.MousePos.y-g_Isl.dragSt.y,dist=sqrtf(dx*dx+dy*dy);
            if(g_Isl.dragSt.x>=0&&!g_Isl.dragS&&dist>dr){g_Isl.dragS=true;g_Isl.drag=true;}
            if(g_Isl.drag){g_Isl.pos=ImVec2(io.MousePos.x-g_Isl.dragOff.x,io.MousePos.y-g_Isl.dragOff.y);
                if(g_Isl.pos.x<r)g_Isl.pos.x=r;if(g_Isl.pos.x>io.DisplaySize.x-r)g_Isl.pos.x=io.DisplaySize.x-r;
                if(g_Isl.pos.y<r)g_Isl.pos.y=r;if(g_Isl.pos.y>io.DisplaySize.y-r)g_Isl.pos.y=io.DisplaySize.y-r;}
        }else{if(g_Isl.drag){g_Isl.drag=false;g_Isl.dragS=false;}else if(g_Isl.dragSt.x>=0&&inC)*clicked=true;g_Isl.dragSt=ImVec2(-1,-1);}
    }else{
        g_Isl.drag=false;g_Isl.dragS=false;g_Isl.dragSt=ImVec2(-1,-1);
        static bool s_islandDown=false;
        if(inC&&io.MouseClicked[0])s_islandDown=true;
        if(s_islandDown&&!io.MouseDown[0]){s_islandDown=false;if(inC)*clicked=true;}
        else if(!inC)s_islandDown=false;
    }
    ImU32 idle=ImGui::ColorConvertFloat4ToU32(Primary),hov=ImGui::ColorConvertFloat4ToU32(PriL),pres=ImGui::ColorConvertFloat4ToU32(PriD);
    ImU32 bg;float ra=0;bool pr=inC&&io.MouseDown[0];
    if(g_Isl.drag||pr){bg=pres;ra=Scale(4);}else if(inC){bg=hov;ra=Scale(3);}else bg=idle;
    dl->AddCircleFilled(ImVec2(c.x,c.y+Scale(6)),r+ra,IM_COL32(0,0,0,100),48);
    dl->AddCircleFilled(c,r+ra,bg,48);dl->AddCircle(c,r+ra,IM_COL32(255,255,255,80),48,Scale(2));
    if(g_FontIsland){const char*l="AI";ImVec2 ts=g_FontIsland->CalcTextSizeA(g_FontIsland->FontSize,FLT_MAX,0,l);
    dl->AddText(g_FontIsland,g_FontIsland->FontSize,ImVec2(c.x-ts.x*0.5f,c.y-ts.y*0.5f),IM_COL32(255,255,255,255),l);}
    if(Config::running)dl->AddCircleFilled(ImVec2(c.x+r*0.6f,c.y-r*0.6f),Scale(8),IM_COL32(80,255,80,255),16);
    return *clicked;
}

static void DrawUI(){
    if(g_UIFont)ImGui::PushFont(g_UIFont);
    ImGuiIO&io=ImGui::GetIO();float ct=ImGui::GetTime(),dt=g_LastT>0?(ct-g_LastT):0.016f;g_LastT=ct;
    bool clk=false;DrawIsland(&clk);if(clk)g_ShowUI=!g_ShowUI;
    ImDrawList*dl=ImGui::GetForegroundDrawList();
    Danmu::Update(dt,(int)io.DisplaySize.x,(int)io.DisplaySize.y,g_DanmuFont);
    Danmu::Render(dl,g_DanmuFont);
    if(g_ShowUI)DrawConfigWin();
    if(g_UIFont)ImGui::PopFont();
}

static void Setup(){
    if(g_Init||g_W<=0||g_H<=0)return;
    LOGI("DanmuGL setup...");ImGui::CreateContext();ImGuiIO&io=ImGui::GetIO();
    io.IniFilename=nullptr;io.LogFilename=nullptr;
    g_Dpi=(float)g_H/900.0f;if(g_Dpi<1.0f)g_Dpi=1.0f;if(g_Dpi>2.5f)g_Dpi=2.5f;
    io.Fonts->Clear();ImFontConfig cfg;cfg.FontDataOwnedByAtlas=false;cfg.OversampleH=cfg.OversampleV=2;cfg.PixelSnapH=true;
    g_FontIsland=io.Fonts->AddFontFromMemoryTTF((void*)inter_medium.data(),(int)inter_medium.size(),Scale(32),&cfg,io.Fonts->GetGlyphRangesDefault());
    g_UIFont=io.Fonts->AddFontFromMemoryTTF((void*)inter_medium.data(),(int)inter_medium.size(),Scale(26),&cfg,io.Fonts->GetGlyphRangesDefault());
    g_DanmuFont=io.Fonts->AddFontFromMemoryTTF((void*)inter_medium.data(),(int)inter_medium.size(),Scale(32),&cfg,io.Fonts->GetGlyphRangesDefault());
    const ImWchar* ranges=io.Fonts->GetGlyphRangesChineseFull();
    if(!Config::font_path.empty()){
        ImFont*fi=io.Fonts->AddFontFromFileTTF(Config::font_path.c_str(),Scale(32),&cfg,ranges);
        ImFont*fu=io.Fonts->AddFontFromFileTTF(Config::font_path.c_str(),Scale(26),&cfg,ranges);
        ImFont*fd=io.Fonts->AddFontFromFileTTF(Config::font_path.c_str(),Scale(32),&cfg,ranges);
        if(fi&&fu&&fd){g_FontIsland=fi;g_UIFont=fu;g_DanmuFont=fd;g_FontMsg="";}
        else{g_FontMsg="Font load failed, using built-in font";}
    }else{g_FontMsg="No Chinese font configured, Chinese danmaku may not display";}
    if(g_UIFont)io.FontDefault=g_UIFont;
    ImGui_ImplAndroid_Init(nullptr);ImGui_ImplOpenGL3_Init("#version 300 es");
    Logger::Init();
    SetupStyle();g_Init=true;g_LastT=ImGui::GetTime();
    if(Config::running)AIClient::Start();
    LOGI("DanmuGL setup complete, DPI: %.2f", g_Dpi);
}

static void RenderUI(){
    if(!g_Init)return;
    if(Config::running)Capture::CaptureOnRenderThread();
    GLSt s;SaveGL(s);ImGuiIO&io=ImGui::GetIO();
    io.DisplaySize=ImVec2((float)g_W,(float)g_H);io.DisplayFramebufferScale=ImVec2(1,1);
    ImGui_ImplOpenGL3_NewFrame();ImGui_ImplAndroid_NewFrame(g_W,g_H);
    ImGui::NewFrame();DrawUI();ImGui::Render();ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(s);
}

static EGLBoolean(*orig_eglSwapBuffers)(EGLDisplay,EGLSurface)=nullptr;
static EGLBoolean hook_eglSwapBuffers(EGLDisplay d,EGLSurface s){
    if(!orig_eglSwapBuffers)return orig_eglSwapBuffers(d,s);
    EGLContext ctx=eglGetCurrentContext();if(ctx==EGL_NO_CONTEXT)return orig_eglSwapBuffers(d,s);
    EGLint w=0,h=0;eglQuerySurface(d,s,EGL_WIDTH,&w);eglQuerySurface(d,s,EGL_HEIGHT,&h);
    if(w<300||h<300)return orig_eglSwapBuffers(d,s);
    if(g_Ctx==EGL_NO_CONTEXT){EGLint b;eglQuerySurface(d,s,EGL_RENDER_BUFFER,&b);if(b==EGL_BACK_BUFFER){g_Ctx=ctx;g_Surf=s;}}
    if(ctx!=g_Ctx||s!=g_Surf){if(g_Init){g_Init=false;g_Ctx=EGL_NO_CONTEXT;g_Surf=EGL_NO_SURFACE;}return orig_eglSwapBuffers(d,s);}
    g_W=w;g_H=h;Setup();RenderUI();return orig_eglSwapBuffers(d,s);
}

static void HandleTouch(int a,int,float x,float y){
    if(!g_Init)return;ImGuiIO&io=ImGui::GetIO();io.AddMousePosEvent(x,y);
    if(a==AMOTION_EVENT_ACTION_DOWN)io.AddMouseButtonEvent(0,true);
    else if(a==AMOTION_EVENT_ACTION_UP||a==AMOTION_EVENT_ACTION_CANCEL)io.AddMouseButtonEvent(0,false);
}

static bool IsPointInAnyWindow(float x,float y){
    if(!g_ShowUI)return false;
    ImGuiContext* ctx=ImGui::GetCurrentContext();if(!ctx)return false;
    for(ImGuiWindow* w:ctx->Windows){
        if(!w->WasActive)continue;
        ImVec2 min=w->Pos,max=ImVec2(w->Pos.x+w->Size.x,w->Pos.y+w->Size.y);
        if(x>=min.x&&x<=max.x&&y>=min.y&&y<=max.y)return true;
    }
    return false;
}

static bool s_touchCaptured=false;
static bool DispatchTouch(int a,int id,float x,float y){
    if(!g_Init)return false;
    bool on_island=InIsland(x,y);
    bool on_window=IsPointInAnyWindow(x,y);
    if(a==AMOTION_EVENT_ACTION_DOWN){
        s_islandTouched=on_island;
        if(on_island||on_window){s_touchCaptured=true;HandleTouch(a,id,x,y);return true;}
        s_touchCaptured=false;return false;
    }
    else if(a==AMOTION_EVENT_ACTION_MOVE){
        if(s_touchCaptured||s_islandTouched){HandleTouch(a,id,x,y);return true;}
        return false;
    }
    else if(a==AMOTION_EVENT_ACTION_UP||a==AMOTION_EVENT_ACTION_CANCEL){
        bool cap=s_touchCaptured||s_islandTouched;
        if(cap)HandleTouch(a,id,x,y);
        s_touchCaptured=false;s_islandTouched=false;return cap;
    }
    return false;
}

static bool OnTouchCb(int a,int b,float c,float d){return DispatchTouch(a,b,c,d);}
static void RegisterPreloader(){
    void*lib=dlopen("libpreloader.so",RTLD_NOW);if(!lib)return;
    GetPreloaderInput_Fn fn=(GetPreloaderInput_Fn)dlsym(lib,"GetPreloaderInput");if(!fn){dlclose(lib);return;}
    PreloaderInput_Interface*iface=fn();if(iface&&iface->RegisterTouchCallback){iface->RegisterTouchCallback(OnTouchCb);g_PreloaderInputAvailable=true;}
    dlclose(lib);
}

static void hook_MECopyFrom(void*self,void*other,void*kh){
    if(orig_MotionEvent_copyFrom)orig_MotionEvent_copyFrom(self,other,kh);
    if(!g_Init||g_PreloaderInputAvailable)return;
    AInputEvent*ev=(AInputEvent*)self;if(AInputEvent_getType(ev)!=AINPUT_EVENT_TYPE_MOTION)return;
    int32_t action=AMotionEvent_getAction(ev);
    int actionMasked=action&AMOTION_EVENT_ACTION_MASK;
    int pointerIndex=(action&AMOTION_EVENT_ACTION_POINTER_INDEX_MASK)>>AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    size_t cnt=AMotionEvent_getPointerCount(ev);
    if(actionMasked==AMOTION_EVENT_ACTION_DOWN||actionMasked==AMOTION_EVENT_ACTION_POINTER_DOWN){int id=AMotionEvent_getPointerId(ev,pointerIndex);DispatchTouch(AMOTION_EVENT_ACTION_DOWN,id,AMotionEvent_getX(ev,pointerIndex),AMotionEvent_getY(ev,pointerIndex));}
    else if(actionMasked==AMOTION_EVENT_ACTION_MOVE){for(size_t i=0;i<cnt;i++)DispatchTouch(AMOTION_EVENT_ACTION_MOVE,AMotionEvent_getPointerId(ev,i),AMotionEvent_getX(ev,i),AMotionEvent_getY(ev,i));}
    else if(actionMasked==AMOTION_EVENT_ACTION_UP||actionMasked==AMOTION_EVENT_ACTION_POINTER_UP){int id=AMotionEvent_getPointerId(ev,pointerIndex);DispatchTouch(AMOTION_EVENT_ACTION_UP,id,AMotionEvent_getX(ev,pointerIndex),AMotionEvent_getY(ev,pointerIndex));}
    else if(actionMasked==AMOTION_EVENT_ACTION_CANCEL){DispatchTouch(AMOTION_EVENT_ACTION_CANCEL,0,0,0);s_islandTouched=false;}
}

static void HookInput(){
    GHandle h=GlossOpen("libinput.so");if(h){uintptr_t sym=GlossSymbol(h,"_ZN7android11MotionEvent8copyFromEPKS0_b",nullptr);if(sym)GlossHook((void*)sym,(void*)hook_MECopyFrom,(void**)&orig_MotionEvent_copyFrom);}
}

static void*MainThread(void*){
    sleep(3);LOGI("DanmuGL loaded");GlossInit(true);
    Config::EnsureConfigDir();Config::LoadConfig();srand((unsigned)time(nullptr));RegisterPreloader();
    GHandle egl=GlossOpen("libEGL.so");if(egl){void*sw=(void*)GlossSymbol(egl,"eglSwapBuffers",nullptr);if(sw)GlossHook(sw,(void*)hook_eglSwapBuffers,(void**)&orig_eglSwapBuffers);}
    HookInput();if(Config::running)AIClient::Start();return nullptr;
}

__attribute__((constructor))void init(){pthread_t t;pthread_create(&t,nullptr,MainThread,nullptr);}
