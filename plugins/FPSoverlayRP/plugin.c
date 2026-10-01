#include "anomaly/sdk/anomaly_sdk.h"

#include <Windows.h>
#include <Psapi.h>
#include <TlHelp32.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HAS_FIELD(p, T, f) ((p) != NULL && (p)->struct_size >= offsetof(T, f) + sizeof((p)->f))

static const AnomalyHostApiV1* g_host;
static const AnomalyStorageServiceV1* g_storage;
static const AnomalyDiagnosticsServiceV1* g_diagnostics;
static uint64_t g_base_memory;
static uint32_t g_base_threads;
static double g_fps;
static double g_frame_ms;
static int g_expanded;
static int g_frame_bad, g_memory_bad, g_thread_bad;
static int g_alert_active;
static int g_alert_acknowledged;
static uint32_t g_sample_tick;
static uint32_t g_frame_bad_streak;
static uint32_t g_memory_bad_streak;
static uint32_t g_thread_bad_streak;
static double g_max_frame_ms;
static uint64_t g_last_memory;
static uint32_t g_last_threads;
static int g_have_diag;
static int g_saved;
static char g_diag[1024 * 1024];
static size_t g_diag_size;
static char g_frame_plugin[160];
static char g_memory_plugin[160];
static char g_thread_plugin[160];

/* Native transparent FPS overlay. It is intentionally separate from the Anomaly UI
   so the normal-state display has no window frame/background. */
static HWND g_fps_overlay;
static HINSTANCE g_module;
static UINT g_overlay_dpi = 96;
static char g_overlay_text[64];
static int g_overlay_created;
static const wchar_t* kOverlayClass = L"AnomalyRuntimeProfilerFpsOverlay";

static AnomalyStringViewV1 sv(const char* s) {
    AnomalyStringViewV1 v = {s, s ? strlen(s) : 0};
    return v;
}
static AnomalyStatusV1 ok(void) { AnomalyStatusV1 s={ANOMALY_STATUS_V1_OK,0,{0,0}}; return s; }
static AnomalyStatusV1 code(uint32_t c) { AnomalyStatusV1 s={c,0,{0,0}}; return s; }
static int succeeded(AnomalyStatusV1 s) { return s.code == ANOMALY_STATUS_V1_OK; }

static const void* query(const AnomalyHostApiV1* host, const char* id, uint32_t version) {
    const void* table = NULL;
    if (!host || !host->query_service) return NULL;
    if (!succeeded(host->query_service(host->host_context, sv(id), version, &table))) return NULL;
    return table;
}

static uint64_t process_memory(void) {
    PROCESS_MEMORY_COUNTERS c;
    memset(&c, 0, sizeof(c)); c.cb = sizeof(c);
    if (!GetProcessMemoryInfo(GetCurrentProcess(), &c, sizeof(c))) return 0;
    return (uint64_t)c.WorkingSetSize;
}
static uint32_t process_threads(void) {
    DWORD pid = GetCurrentProcessId();
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 e;
    uint32_t n=0;
    if (h == INVALID_HANDLE_VALUE) return 0;
    memset(&e,0,sizeof(e)); e.dwSize=sizeof(e);
    if (Thread32First(h,&e)) {
        do { if (e.th32OwnerProcessID == pid) ++n; e.dwSize=sizeof(e); } while (Thread32Next(h,&e));
    }
    CloseHandle(h); return n;
}
static void format_mib(char* out, size_t cap, uint64_t bytes) {
    snprintf(out, cap, "%.1f MiB", (double)bytes / (1024.0*1024.0));
}
static void format_ms(char* out, size_t cap, double ms) { snprintf(out, cap, "%.1f ms", ms); }

static const char* skip_ws(const char* p, const char* end) {
    while (p<end && (*p==' '||*p=='\t'||*p=='\r'||*p=='\n'||*p==',')) ++p;
    return p;
}
static const char* find_key(const char* obj, const char* end, const char* key) {
    char needle[96]; int n=snprintf(needle,sizeof(needle),"\"%s\"",key);
    const char* p=obj;
    if(n<=0 || (size_t)n>=sizeof(needle)) return NULL;
    while (p<end) {
        const char* q=(const char*)memchr(p,'\"',(size_t)(end-p));
        if(!q) return NULL;
        if((size_t)(end-q)<(size_t)n) return NULL;
        if(memcmp(q,needle,(size_t)n)==0) return q+n;
        p=q+1;
    }
    return NULL;
}
static uint64_t uint_field(const char* obj, const char* end, const char* key) {
    const char* p=find_key(obj,end,key); char* ep; unsigned long long v;
    if(!p) return 0; p=memchr(p,':',(size_t)(end-p)); if(!p) return 0; ++p; p=skip_ws(p,end);
    v=strtoull(p,&ep,10); if(ep==p || ep>end) return 0; return (uint64_t)v;
}
static int string_field(const char* obj, const char* end, const char* key, char* out, size_t cap) {
    const char* p=find_key(obj,end,key); size_t n=0;
    if(!p || cap==0) return 0;
    p=memchr(p,':',(size_t)(end-p)); if(!p) return 0; ++p; p=skip_ws(p,end);
    if(p>=end || *p!='\"') return 0; ++p;
    while(p<end && *p!='\"' && n+1<cap) {
        if(*p=='\\' && p+1<end) { ++p; if(*p=='n') out[n++]=' '; else out[n++]=*p; ++p; }
        else out[n++]=*p++;
    }
    out[n]=0; return n!=0;
}
static const char* matching_object_end(const char* p, const char* end) {
    int depth=0,in_str=0,esc=0;
    for(;p<end;++p){
        char c=*p;
        if(in_str){ if(esc) esc=0; else if(c=='\\') esc=1; else if(c=='\"') in_str=0; continue; }
        if(c=='\"'){in_str=1;continue;} if(c=='{') ++depth; else if(c=='}'){--depth;if(depth==0)return p+1;}
    }
    return NULL;
}
static void clear_plugins(void) { g_frame_plugin[0]=g_memory_plugin[0]=g_thread_plugin[0]=0; }

static int overlay_dpi_scale(int value) {
    return (int)(((int64_t)value * (int64_t)g_overlay_dpi + 48) / 96);
}

static void overlay_paint(HWND hwnd) {
    if (!hwnd || !g_overlay_created) return;

    RECT rc; GetClientRect(hwnd, &rc);
    const int width = rc.right - rc.left;
    const int height = rc.bottom - rc.top;
    if (width <= 0 || height <= 0) return;

    HDC screen = GetDC(NULL);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi; memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = width;
    bi.bmiHeader.biHeight = -height;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = NULL;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp || !bits) {
        if (bmp) DeleteObject(bmp);
        DeleteDC(mem); ReleaseDC(NULL, screen); return;
    }
    HGDIOBJ old = SelectObject(mem, bmp);
    memset(bits, 0, (size_t)width * (size_t)height * 4U);

    SetBkMode(mem, TRANSPARENT);
    HFONT font = CreateFontW(overlay_dpi_scale(20), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    HGDIOBJ old_font = font ? SelectObject(mem, font) : NULL;

    wchar_t wide[64];
    int converted = MultiByteToWideChar(CP_UTF8, 0, g_overlay_text, -1, wide, 64);
    if (converted <= 0) wide[0] = L'\0';

    RECT text_rc = {overlay_dpi_scale(2), overlay_dpi_scale(2), width - overlay_dpi_scale(2), height - overlay_dpi_scale(2)};
    /* Subtle shadow keeps the text readable over both dark and bright scenes. */
    SetTextColor(mem, RGB(0, 0, 0));
    RECT shadow = text_rc; OffsetRect(&shadow, overlay_dpi_scale(1), overlay_dpi_scale(1));
    DrawTextW(mem, wide, -1, &shadow, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    SetTextColor(mem, RGB(245, 245, 245));
    DrawTextW(mem, wide, -1, &text_rc, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

    if (old_font) SelectObject(mem, old_font);
    if (font) DeleteObject(font);
    SelectObject(mem, old);

    POINT src = {0, 0};
    POINT pos; RECT wr; GetWindowRect(hwnd, &wr);
    pos.x = wr.left; pos.y = wr.top;
    SIZE size = {width, height};
    BLENDFUNCTION blend = {AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    UpdateLayeredWindow(hwnd, screen, &pos, &size, mem, &src, 0, &blend, ULW_ALPHA);

    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(NULL, screen);
}

static LRESULT CALLBACK overlay_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    (void)wp; (void)lp;
    if (msg == WM_DPICHANGED) {
        g_overlay_dpi = HIWORD(wp);
        RECT* suggested = (RECT*)lp;
        if (suggested) SetWindowPos(hwnd, HWND_TOPMOST, suggested->left, suggested->top,
            suggested->right - suggested->left, suggested->bottom - suggested->top,
            SWP_NOACTIVATE | SWP_SHOWWINDOW);
        overlay_paint(hwnd);
        return 0;
    }
    if (msg == WM_ERASEBKGND) return 1;
    if (msg == WM_NCHITTEST) return HTTRANSPARENT;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static void overlay_hide(void) {
    if (g_fps_overlay) ShowWindow(g_fps_overlay, SW_HIDE);
}

static void overlay_destroy(void) {
    if (g_fps_overlay) DestroyWindow(g_fps_overlay);
    g_fps_overlay = NULL; g_overlay_created = 0;
}

static void overlay_create(void) {
    if (g_fps_overlay || !g_module) return;
    WNDCLASSW wc; memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = overlay_proc; wc.hInstance = g_module; wc.lpszClassName = kOverlayClass;
    wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
    RegisterClassW(&wc);
    g_overlay_dpi = GetDpiForSystem(); if (!g_overlay_dpi) g_overlay_dpi = 96;
    const int w = overlay_dpi_scale(180);
    const int h = overlay_dpi_scale(38);
    const int x = overlay_dpi_scale(20);
    const int y = overlay_dpi_scale(20);
    g_fps_overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kOverlayClass, L"", WS_POPUP, x, y, w, h, NULL, NULL, g_module, NULL);
    if (!g_fps_overlay) return;
    g_overlay_created = 1;
    SetWindowPos(g_fps_overlay, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

static int overlay_process_is_foreground(void) {
    HWND fg = GetForegroundWindow();
    if (!fg) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    return pid == GetCurrentProcessId();
}

static void overlay_update(void) {
    if (!g_fps_overlay || g_alert_active || g_alert_acknowledged) return;
    if (!overlay_process_is_foreground()) {
        ShowWindow(g_fps_overlay, SW_HIDE);
        return;
    }
    ShowWindow(g_fps_overlay, SW_SHOWNOACTIVATE);
    char next[64];
    snprintf(next, sizeof(next), "FPS %.1f", g_fps);
    if (strcmp(next, g_overlay_text) == 0) return;
    snprintf(g_overlay_text, sizeof(g_overlay_text), "%s", next);
    overlay_paint(g_fps_overlay);
}

static void overlay_pump(void) {
    if (!g_fps_overlay) return;
    MSG msg;
    while (PeekMessageW(&msg, g_fps_overlay, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
}

static void snapshot_diagnostics(void) {
    g_have_diag=0; g_diag_size=0; clear_plugins();
    if(!g_diagnostics || !g_diagnostics->snapshot_json) return;
    size_t need=0;
    AnomalyStatusV1 s=g_diagnostics->snapshot_json(g_diagnostics->user,(AnomalyMutableByteSpanV1){NULL,0},&need);
    if(s.code!=ANOMALY_STATUS_V1_OK && s.code!=ANOMALY_STATUS_V1_BUFFER_TOO_SMALL) return;
    if(need==0 || need>=sizeof(g_diag)) return;
    size_t actual=need;
    s=g_diagnostics->snapshot_json(g_diagnostics->user,(AnomalyMutableByteSpanV1){(uint8_t*)g_diag,sizeof(g_diag)},&actual);
    if(!succeeded(s)) return;
    if(actual>=sizeof(g_diag)) actual=sizeof(g_diag)-1; g_diag[actual]=0; g_diag_size=actual; g_have_diag=1;

    const char* root=g_diag; const char* end=g_diag+g_diag_size; const char* p=root;
    const char* plugins=find_key(root,end,"plugins");
    if(!plugins) return; plugins=memchr(plugins,'[',(size_t)(end-plugins)); if(!plugins) return; ++plugins;
    uint64_t bf=0,bm=0,bt=0;
    while(plugins<end && *plugins!=']') {
        plugins=skip_ws(plugins,end); if(plugins>=end||*plugins==']')break; if(*plugins!='{'){++plugins;continue;}
        const char* oe=matching_object_end(plugins,end); if(!oe)break;
        char id[160]={0}; string_field(plugins,oe,"id",id,sizeof(id));
        if(id[0] && strcmp(id,"anomaly.tools.runtime-profiler")!=0){
            const char* cb=find_key(plugins,oe,"callbacks"); uint64_t fs=0;
            if(cb){ cb=memchr(cb,'{',(size_t)(oe-cb)); if(cb){const char* ce=matching_object_end(cb,oe);if(ce)fs=uint_field(cb,ce,"updateSlow")+uint_field(cb,ce,"drawSlow");}}
            const char* rs=find_key(plugins,oe,"resources"); uint64_t ms=0,ts=0;
            if(rs){ rs=memchr(rs,'{',(size_t)(oe-rs)); if(rs){const char* re=matching_object_end(rs,oe);if(re){ts=uint_field(rs,re,"tasks");ms=ts+uint_field(rs,re,"hooks")+uint_field(rs,re,"patches")+uint_field(rs,re,"windows");}}}
            if(fs>bf){bf=fs;snprintf(g_frame_plugin,sizeof(g_frame_plugin),"%s",id);}
            if(ms>bm){bm=ms;snprintf(g_memory_plugin,sizeof(g_memory_plugin),"%s",id);}
            if(ts>bt){bt=ts;snprintf(g_thread_plugin,sizeof(g_thread_plugin),"%s",id);}
        }
        p=oe; plugins=p;
    }
}

static void recalc(double delta) {
    /* The hot path is intentionally tiny: do not enumerate threads or copy
       the diagnostics JSON every frame. Frame timing is observed every update,
       while expensive process sampling happens roughly once per second. */
    g_frame_ms = (delta > 0.0 && delta < 10.0) ? delta * 1000.0 : 0.0;
    g_fps = (delta > 0.0 && delta < 10.0) ? 1.0 / delta : 0.0;
    if (g_frame_ms > g_max_frame_ms) g_max_frame_ms = g_frame_ms;

    const int frame_sample_bad = (g_frame_ms >= 20.0 || (g_fps > 0.0 && g_fps < 50.0));
    if (frame_sample_bad) ++g_frame_bad_streak;
    else g_frame_bad_streak = 0;
    if (g_frame_bad_streak >= 5) g_frame_bad = 1;

    ++g_sample_tick;
    if (g_sample_tick < 60) return;
    g_sample_tick = 0;

    const uint64_t mem = process_memory();
    const uint32_t th = process_threads();
    if (!g_base_memory) g_base_memory = mem;
    if (!g_base_threads) g_base_threads = th;
    g_last_memory = mem;
    g_last_threads = th;

    const int memory_bad = (mem >= g_base_memory &&
        mem - g_base_memory >= 64ULL * 1024ULL * 1024ULL);
    const int thread_bad = (th >= g_base_threads && th - g_base_threads >= 8U);

    if (memory_bad) ++g_memory_bad_streak;
    else g_memory_bad_streak = 0;
    if (thread_bad) ++g_thread_bad_streak;
    else g_thread_bad_streak = 0;

    if (g_memory_bad_streak >= 2) g_memory_bad = 1;
    if (g_thread_bad_streak >= 2) g_thread_bad = 1;

    if ((g_frame_bad || g_memory_bad || g_thread_bad) && !g_alert_active) {
        g_alert_active = 1;
        g_alert_acknowledged = 0;
        /* Diagnostics are fetched by update() after the alert state is established. */
    }
}

static void save_log(void) {
    if(!g_storage || !g_storage->write_atomic || !g_diag_size) return;
    SYSTEMTIME t; char path[128]; GetLocalTime(&t);
    snprintf(path,sizeof(path),"profiling/runtime-profiler-%04u%02u%02u-%02u%02u%02u.json",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond);
    AnomalyByteSpanV1 bytes={(const uint8_t*)g_diag,g_diag_size};
    g_saved=succeeded(g_storage->write_atomic(g_storage->user,sv(path),bytes));
}

static AnomalyStatusV1 ANOMALY_CALL load(const AnomalyHostApiV1* host, void** context) {
    if(!host || !context) return code(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *context=NULL;
    const AnomalyUiServiceV1* ui=(const AnomalyUiServiceV1*)query(host,ANOMALY_UI_SERVICE_V1_ID,ANOMALY_UI_SERVICE_V1_VERSION);
    if(!ui || !HAS_FIELD(ui,AnomalyUiServiceV1,text) || !ui->text || !ui->begin_window || !ui->end_window) return code(ANOMALY_STATUS_V1_UNAVAILABLE);
    g_host=host;
    g_module = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCWSTR)(const void*)&load, &g_module);
    g_fps_overlay = NULL; g_overlay_created = 0; g_overlay_text[0] = 0;
    g_storage=(const AnomalyStorageServiceV1*)query(host,"anomaly.storage",1);
    g_diagnostics=(const AnomalyDiagnosticsServiceV1*)query(host,"anomaly.diagnostics",1);
    g_base_memory=0; g_base_threads=0; g_fps=0; g_frame_ms=0; g_expanded=0; g_saved=0; g_have_diag=0; g_diag_size=0;
    g_alert_active=0; g_alert_acknowledged=0; g_sample_tick=0;
    g_frame_bad_streak=0; g_memory_bad_streak=0; g_thread_bad_streak=0;
    g_max_frame_ms=0; g_last_memory=0; g_last_threads=0;
    clear_plugins();
    *context=(void*)1;
    return ok();
}
static AnomalyStatusV1 ANOMALY_CALL start(void* context) {
    (void)context;
    g_base_memory=process_memory();
    g_base_threads=process_threads();
    g_last_memory=g_base_memory;
    g_last_threads=g_base_threads;
    g_sample_tick=0;
    return ok();
}
static AnomalyStatusV1 ANOMALY_CALL stop(void* context,uint32_t deadline){(void)context;(void)deadline;return ok();}
static void ANOMALY_CALL unload(void* context){(void)context;overlay_destroy();g_host=NULL;g_storage=NULL;g_diagnostics=NULL;g_diag_size=0;}
static void ANOMALY_CALL update(void* context,double delta){
    (void)context;
    recalc(delta);
    if (!g_fps_overlay) overlay_create();
    overlay_pump();
    if (g_alert_active) overlay_hide();
    else overlay_update();
    if(g_alert_active && g_host){
        /* The first alert is the only time we ask the host for the expensive
           diagnostics snapshot. Normal operation does not touch diagnostics. */
        if(!g_diagnostics)
            g_diagnostics=(const AnomalyDiagnosticsServiceV1*)query(g_host,"anomaly.diagnostics",1);
        if(!g_storage)
            g_storage=(const AnomalyStorageServiceV1*)query(g_host,"anomaly.storage",1);
        if(!g_have_diag) snapshot_diagnostics();
    }
}
static void text(const AnomalyUiServiceV1* ui,const char* s){ if(ui&&ui->text)ui->text(ui->user,sv(s)); }
static void ANOMALY_CALL draw(void* context,const AnomalyUiServiceV1* ui){
    (void)context;
    if (!ui || !ui->begin_window || !ui->end_window) return;

    /* Normal state uses a native layered overlay: no frame, no background, DPI-scaled text. */
    if (!g_alert_active || g_alert_acknowledged) return;

    int open=1;
    int visible=ui->begin_window(ui->user,sv("【运行异常监测】"),&open,0);
    if(!visible){ ui->end_window(ui->user); return; }

    char line[512],a[64],b[64];
    text(ui,"检测到运行异常，请检查以下项目：");
    if(g_frame_bad){
        format_ms(a,sizeof(a),g_max_frame_ms);
        snprintf(line,sizeof(line),"画面延时 / 帧数下降：峰值 %s%s%s",a,
            g_frame_plugin[0]?"；疑似相关插件：":"；暂未定位到插件",g_frame_plugin);
        text(ui,line);
    }
    if(g_memory_bad){
        format_mib(a,sizeof(a),g_base_memory);
        format_mib(b,sizeof(b),g_last_memory);
        snprintf(line,sizeof(line),"内存异常增加：基线 %s / 当前 %s%s%s",a,b,
            g_memory_plugin[0]?"；疑似相关插件：":"；暂未定位到插件",g_memory_plugin);
        text(ui,line);
    }
    if(g_thread_bad){
        snprintf(line,sizeof(line),"线程异常增加：基线 %u / 当前 %u%s%s",g_base_threads,g_last_threads,
            g_thread_plugin[0]?"；疑似相关插件：":"；暂未定位到插件",g_thread_plugin);
        text(ui,line);
    }
    if(g_frame_plugin[0]){snprintf(line,sizeof(line),"画面疑似插件：%s",g_frame_plugin);text(ui,line);}
    if(g_memory_plugin[0]){snprintf(line,sizeof(line),"资源疑似插件：%s",g_memory_plugin);text(ui,line);}
    if(g_thread_plugin[0]){snprintf(line,sizeof(line),"线程疑似插件：%s",g_thread_plugin);text(ui,line);}

    if(ui->button && ui->button(ui->user,sv("保存当前原始监测日志"),0,0)) save_log();
    if(ui->button && ui->button(ui->user,sv("知道了，关闭提示"),0,0)) {
        g_alert_acknowledged=1;
        g_alert_active=0;
        g_frame_bad=0; g_memory_bad=0; g_thread_bad=0;
        g_frame_bad_streak=0; g_memory_bad_streak=0; g_thread_bad_streak=0;
        g_have_diag=0; g_diag_size=0;
        g_max_frame_ms=0;
        overlay_update();
    }
    ui->end_window(ui->user);
}
ANOMALY_SDK_EXPORT AnomalyStatusV1 ANOMALY_CALL AnomalyPluginEntryV1(AnomalyPluginDescriptorV1* d){
    if(!d || d->struct_size<sizeof(*d)) return code(ANOMALY_STATUS_V1_INVALID_ARGUMENT);
    *d=(AnomalyPluginDescriptorV1){sizeof(*d),ANOMALY_PLUGIN_API_V1_MAJOR,ANOMALY_PLUGIN_API_V1_MINOR,
        sv("anomaly.tools.runtime-profiler"),sv("运行异常监测"),sv("Anomaly"),sv("0.10.2"),
        load,start,stop,unload,update,draw};
    return ok();
}
