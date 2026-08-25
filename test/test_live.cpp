// End-to-end: real search + real player through the full new stack.
#include "yt_innertube.h"
#include <chrono>
#include <cstdio>
using namespace ytfast;
static const char* sv(std::string_view s){static char b[128];size_t n=s.size()<127?s.size():127;memcpy(b,s.data(),n);b[n]=0;return b;}
int main(int argc,char**argv){
    CurlGlobalInit init;
    auto& yt = InnertubeClient::get_instance();
    auto t0=std::chrono::steady_clock::now();
    yt.bootstrap_visitor_data();
    auto el=[&]{return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();};
    std::string vd=yt.visitor_data();
    std::printf("visitor_data: %zu chars  (%.0f ms)\n", vd.size(), el());

    std::string q = argc>1?argv[1]:"lofi hip hop";
    t0=std::chrono::steady_clock::now();
    auto res=yt.search(q,5,false);
    std::printf("\nsearch \"%s\" -> %zu results (%.0f ms)\n",q.c_str(),res.size(),el());
    for(auto&r:res) std::printf("  [%s] %-46s | %-20s | %s\n",r.id.c_str(),
        r.title.substr(0,46).c_str(),r.channel.substr(0,20).c_str(),r.duration_str.c_str());
    if(res.empty()) return 1;

    std::string vid = argc>2?argv[2]:res[0].id;
    t0=std::chrono::steady_clock::now();
    auto info=yt.get_stream_formats(vid);
    std::printf("\nplayer %s -> %zu formats (%.0f ms)\n",vid.c_str(),info.formats.size(),el());
    std::printf("  title: %s\n  channel: %s  duration: %s\n",
        info.title.c_str(),info.channel.c_str(),info.duration_str.c_str());
    if(info.formats.empty()) return 1;

    auto*a=InnertubeClient::pick_audio(info.formats);
    auto*v=InnertubeClient::pick_video(info.formats);
    auto*v720=InnertubeClient::pick_video(info.formats,720);
    auto*m=InnertubeClient::pick_muxed(info.formats);
    if(a)std::printf("  BEST AUDIO itag %-4d %-8s %-5s %lld bps %dch drc=%d\n",a->itag,sv(a->quality_label),sv(a->container),(long long)a->effective_bitrate(),a->audio_channels,a->is_drc);
    if(v)std::printf("  BEST VIDEO itag %-4d %-8s %-5s %dx%d@%d %lld bps\n",v->itag,sv(v->quality_label),sv(v->container),v->width,v->height,v->fps,(long long)v->effective_bitrate());
    if(v720)std::printf("  <=720p     itag %-4d %-8s %dx%d@%d\n",v720->itag,sv(v720->quality_label),v720->width,v720->height,v720->fps);
    if(m)std::printf("  MUXED(old) itag %-4d %-8s %dx%d\n",m->itag,sv(m->quality_label),m->width,m->height);

    // cache hit should be instant
    t0=std::chrono::steady_clock::now();
    auto again=yt.get_stream_formats(vid);
    std::printf("  cache hit: %zu formats in %.2f ms\n",again.formats.size(),el());
    std::printf("\n  audio url: %.78s...\n", a?a->url.c_str():"");
    return 0;
}
