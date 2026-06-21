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
#include <ctime>
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

namespace Config {
const char* CONFIG_PATH = "/storage/emulated/0/games/DanmuGL/config.json";
std::string api_key="",api_base="http://localhost:8000/v1/chat/completions",model_name="gpt-4-vision-preview",font_path="";
int capture_interval=5,max_danmu_count=50; float danmu_speed=150.0f; bool running=false;
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
    if(j.contains("danmu_speed"))danmu_speed=j["danmu_speed"];
    if(j.contains("running"))running=j["running"];
    return true;
}
bool SaveConfig(){
    EnsureConfigDir(); nlohmann::json j;
    j["api_key"]=api_key;j["api_base"]=api_base;j["model_name"]=model_name;j["font_path"]=font_path;
    j["capture_interval"]=capture_interval;j["max_danmu_count"]=max_danmu_count;j["danmu_speed"]=danmu_speed;j["running"]=running;
    std::ofstream f(CONFIG_PATH); if(!f.is_open())return false; f<<j.dump(4); f.close(); return true;
}
}

namespace Danmu {
struct Item{std::string text;float x,y,speed;ImU32 color;float w,h,lifetime;};
std::vector<Item> list; std::mutex mtx;
ImU32 cols[]={IM_COL32_WHITE,IM_COL32(255,200,100,255),IM_COL32(100,255,200,255),IM_COL32(255,150,150,255),IM_COL32(150,200,255,255),IM_COL32(255,255,100,255),IM_COL32(200,255,150,255)};
int cc=7;
void Add(const std::string& t){if(t.empty())return;Item it;it.text=t;it.speed=Config::danmu_speed+(float)(rand()%100-50);it.color=cols[rand()%cc];it.lifetime=0;std::lock_guard<std::mutex> lk(mtx);if(list.size()>=(size_t)Config::max_danmu_count)list.erase(list.begin());list.push_back(it);}
void Update(float dt,int sw,int sh,ImFont* f){
    std::lock_guard<std::mutex> lk(mtx); float lh=f?f->FontSize:24; int ml=(int)(sh/(lh*1.5f));
    for(auto it=list.begin();it!=list.end();){
        Item& d=*it; ImVec2 ts=f?f->CalcTextSizeA(f->FontSize,FLT_MAX,0,d.text.c_str()):ImGui::CalcTextSize(d.text.c_str());
        d.w=ts.x;d.h=ts.y;
        if(d.lifetime==0){d.x=(float)sw+10;int li=rand()%std::max(1,ml);d.y=(float)li*lh*1.5f+lh;if(d.y>sh-lh)d.y=(float)sh-lh-20;}
        d.x-=d.speed*dt;d.lifetime+=dt;
        if(d.x+d.w<-10)it=list.erase(it);else ++it;
    }
}
void Render(ImDrawList* dl,ImFont* f){
    std::lock_guard<std::mutex> lk(mtx); ImFont* rf=f?f:ImGui::GetFont(); float fs=rf->FontSize;
    for(auto& d:list){ImVec2 p(d.x,d.y);ImVec2 ts=rf->CalcTextSizeA(fs,FLT_MAX,0,d.text.c_str());dl->AddRectFilled(ImVec2(p.x-4,p.y-2),ImVec2(p.x+ts.x+4,p.y+ts.y+2),IM_COL32(0,0,0,120),4);dl->AddText(rf,fs,p,d.color,d.text.c_str());}
}
}

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
                if(++attempts > 100) { *err = "SSL handshake timeout"; return -1; }
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
        while(true){
            int n;
            if(https)n=mbedtls_ssl_read(&ssl,(unsigned char*)buf,len);
            else n=(int)recv(net.fd,buf,(int)len,0);
            if(n>0)return n;
            if(n==0)return 0;
            if(https&&(n==MBEDTLS_ERR_SSL_WANT_READ||n==MBEDTLS_ERR_SSL_WANT_WRITE))continue;
            if(!https&&n<0&&(errno==EINTR||errno==EAGAIN))continue;
            return -1;
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

std::string RequestRaw(const std::string& url,const std::string& method,const std::string& body,const std::string& key, const char** err, int* status_code) {
    Url p=ParseUrl(url);
    Connection conn;
    int ret = conn.Connect(p, err);
    if (ret != 0) return "";
    std::ostringstream req;
    req<<method<<" "<<p.path<<" HTTP/1.1\r\nHost: "<<p.host;
    if((!p.https&&p.port!=80)||(p.https&&p.port!=443))req<<":"<<p.port;
    req<<"\r\nContent-Type: application/json\r\nContent-Length: "<<body.size()<<"\r\n";
    if(!key.empty())req<<"Authorization: Bearer "<<key<<"\r\n";
    req<<"Connection: close\r\n\r\n"<<body;
    std::string rs=req.str();
    if(conn.SendAll(rs.c_str(),rs.size())<=0){conn.Close();if(err)*err="Send failed";return "";}
    const size_t MAX_RESP = 2*1024*1024;
    std::string resp;char buf[8192];int n;
    while((n=conn.RecvSome(buf,sizeof(buf)-1))>0){
        buf[n]=0;resp.append(buf,n);
        if(resp.size()>MAX_RESP){if(err)*err="Response too large";return "";}
    }
    conn.Close();
    if(n<0){if(err)*err="Recv failed";return "";}
    size_t he=resp.find("\r\n\r\n");
    if(he==std::string::npos){if(err)*err="Bad response";return "";}
    std::string headers=resp.substr(0,he);
    std::string body_resp=resp.substr(he+4);
    if(status_code){
        size_t sp=headers.find(' ');
        if(sp!=std::string::npos){*status_code=atoi(headers.substr(sp+1).c_str());}
    }
    size_t cl_pos=headers.find("Content-Length:");
    if(cl_pos==std::string::npos)cl_pos=headers.find("content-length:");
    if(cl_pos!=std::string::npos){
        size_t start=headers.find_first_of("0123456789",cl_pos);
        size_t end=headers.find("\r\n",start);
        if(start!=std::string::npos&&end!=std::string::npos){
            long cl=atol(headers.substr(start,end-start).c_str());
            if(cl>0&&cl<(long)MAX_RESP&&(long)body_resp.size()>cl)body_resp=body_resp.substr(0,(size_t)cl);
        }
    }
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
    if(w<=0||h<=0||w>4096||h>4096)return;
    std::vector<unsigned char> px((size_t)w*h*4);
    GLint fbo;glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
    glReadPixels(x,y,w,h,GL_RGBA,GL_UNSIGNED_BYTE,px.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER,fbo);
    for(int r=0;r<h/2;r++){
        for(int c=0;c<w*4;c++){
            std::swap(px[r*w*4+c],px[(h-1-r)*w*4+c]);
        }
    }
    int nw=w,nh=h,md=512;
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
    if(!stbi_write_jpg_to_func([](void* ctx,void* data,int sz){auto*v=(std::vector<unsigned char>*)ctx;v->insert(v->end(),(unsigned char*)data,(unsigned char*)data+sz);},&out,w,h,4,px.data(),70))return;
    if(out.empty())return;
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
std::string ParseDanmu(const std::string& resp){
    try{auto j=nlohmann::json::parse(resp);
    if(j.contains("choices")&&j["choices"].is_array()&&j["choices"].size()>0){auto&c=j["choices"][0];
    if(c.contains("message")&&c["message"].contains("content")){std::string s=c["message"]["content"].get<std::string>();
    size_t a=s.find_first_not_of(" \n\r\t\"'"),b=s.find_last_not_of(" \n\r\t\"'");
    if(a!=std::string::npos&&b!=std::string::npos)s=s.substr(a,b-a+1);if(s.size()>30)s=s.substr(0,30);return s;}}}catch(...){}
    return"";
}
void* Worker(void*){
    while(run){
        {pthread_mutex_lock(&mtx);timespec ts;clock_gettime(CLOCK_REALTIME,&ts);ts.tv_sec+=1;pthread_cond_timedwait(&cond,&mtx,&ts);pthread_mutex_unlock(&mtx);}
        if(!run||!Config::running)continue;time_t now=time(nullptr);if(now-last<Config::capture_interval)continue;last=now;
        if(Config::api_key.empty()&&Config::api_base.find("localhost")==std::string::npos)continue;
        std::vector<unsigned char> jpg;if(!Capture::GetLatestFrame(jpg)||jpg.empty())continue;
        std::string b64=Base64Encode(jpg.data(),jpg.size());
        nlohmann::json req;req["model"]=Config::model_name;req["max_tokens"]=50;req["temperature"]=0.8f;
        nlohmann::json msgs=nlohmann::json::array();
        nlohmann::json sys;sys["role"]="system";sys["content"]="You are a live chat commentator. Generate ONE short, fun comment (max 20 chars) about the game screen in the image. Like a real-time bullet comment on video sites. Just output the comment text directly, no quotes.";msgs.push_back(sys);
        nlohmann::json usr;usr["role"]="user";nlohmann::json ca=nlohmann::json::array();
        nlohmann::json tp;tp["type"]="text";tp["text"]="Generate one fun bullet comment for this game scene.";ca.push_back(tp);
        nlohmann::json ip;ip["type"]="image_url";ip["image_url"]["url"]="data:image/jpeg;base64,"+b64;ca.push_back(ip);
        usr["content"]=ca;msgs.push_back(usr);req["messages"]=msgs;
        std::string body=req.dump();std::string resp=HttpClient::Request(Config::api_base,"POST",body,Config::api_key);
        if(!resp.empty()){std::string txt=ParseDanmu(resp);if(!txt.empty())Danmu::Add(txt);}
    }
    return nullptr;
}
void Start(){if(thr)return;run=true;last=time(nullptr)-Config::capture_interval;pthread_create(&thr,nullptr,Worker,nullptr);}
void Stop(){run=false;if(thr){pthread_cond_signal(&cond);pthread_join(thr,nullptr);thr=0;}}
}

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
static bool InIsland(float x,float y){if(!g_Init||g_ShowUI||g_Isl.pos.x<0)return false;return InCircle(x,y,g_Isl.pos,Scale(56));}

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
    ImGui::SetNextWindowSize(ImVec2(Scale(520),Scale(720)),ImGuiCond_FirstUseEver);ImGuiIO&io=ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x*0.5f,io.DisplaySize.y*0.5f),ImGuiCond_FirstUseEver,ImVec2(0.5f,0.5f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(Scale(24),Scale(24)));ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,Scale(20));
    bool open=true;ImGui::Begin("DanmuGL Settings",&open,ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoCollapse);
    if(!open)g_ShowUI=false;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,Scale(10));
    static char b1[512]={0},b2[512]={0},b3[128]={0},b4[512]={0};
    strncpy(b1,Config::api_key.c_str(),sizeof(b1)-1);b1[sizeof(b1)-1]=0;
    ImGui::TextColored(Primary,"API Configuration");ImGui::Separator();ImGui::Spacing();
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
    ImGui::SliderInt("Capture Interval (sec)",&Config::capture_interval,1,30);ImGui::Spacing();
    ImGui::SliderInt("Max Danmaku Count",&Config::max_danmu_count,10,200);ImGui::Spacing();
    ImGui::SliderFloat("Danmaku Speed",&Config::danmu_speed,50,400,"%.0f");ImGui::Spacing();
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
    if(g_Isl.pos.x<0)g_Isl.pos=ImVec2(io.DisplaySize.x-r-Scale(40),Scale(120));
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
    }else{g_Isl.drag=false;g_Isl.dragS=false;g_Isl.dragSt=ImVec2(-1,-1);}
    ImU32 idle=ImGui::ColorConvertFloat4ToU32(Primary),hov=ImGui::ColorConvertFloat4ToU32(PriL),pres=ImGui::ColorConvertFloat4ToU32(PriD);
    ImU32 bg;float ra=0;bool pr=inC&&io.MouseDown[0]&&!g_Isl.dragS&&!g_ShowUI;
    if(g_Isl.drag||pr){bg=pres;ra=Scale(4);}else if(inC&&!g_ShowUI){bg=hov;ra=Scale(3);}else bg=idle;
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
    SetupStyle();g_Init=true;g_LastT=ImGui::GetTime();
    if(Config::running)AIClient::Start();
    LOGI("DanmuGL setup complete, DPI: %.2f", g_Dpi);
}

static void RenderUI(){
    if(!g_Init)return;GLSt s;SaveGL(s);ImGuiIO&io=ImGui::GetIO();
    io.DisplaySize=ImVec2((float)g_W,(float)g_H);io.DisplayFramebufferScale=ImVec2(1,1);
    ImGui_ImplOpenGL3_NewFrame();ImGui_ImplAndroid_NewFrame(g_W,g_H);
    ImGui::NewFrame();DrawUI();ImGui::Render();ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    RestoreGL(s);
    if(Config::running)Capture::CaptureOnRenderThread();
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

static bool DispatchTouch(int a,int id,float x,float y){
    if(!g_Init)return false;
    if(g_ShowUI){HandleTouch(a,id,x,y);return true;}
    if(a==AMOTION_EVENT_ACTION_DOWN){if(InIsland(x,y)){s_islandTouched=true;HandleTouch(a,id,x,y);return true;}s_islandTouched=false;}
    else if(a==AMOTION_EVENT_ACTION_MOVE){if(s_islandTouched){HandleTouch(a,id,x,y);return true;}}
    else if(a==AMOTION_EVENT_ACTION_UP||a==AMOTION_EVENT_ACTION_CANCEL){if(s_islandTouched){HandleTouch(a,id,x,y);s_islandTouched=false;return true;}s_islandTouched=false;}
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
