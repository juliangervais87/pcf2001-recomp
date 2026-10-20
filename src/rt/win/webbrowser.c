/* El control WebBrowser de Internet Explorer (CLSID 8856f961-…), como lo aloja MFC (CHtmlView / CWnd::CreateControl).
 * PC Fútbol 2001 lo usa para su panel de ayuda: en cada pantalla le carga una página de Ayuda\*.htm. Acá es un control
 * que acepta todo lo que MFC y el juego le piden y no dibuja nada (el panel queda vacío); Navigate anota la página.
 *
 * Un solo objeto con varias caras (cada interfaz es un objeto COM aparte que comparte el estado; QueryInterface pasa de
 * una a otra). Cada método tiene su cantidad exacta de argumentos (stdcall). Lo que falta para con su nombre (com.c) y una
 * interfaz desconocida se rechaza y se anota. */
#include "com.h"

#define E_NOTIMPL_ 0x80004001u
#define S_FALSE 1u
#define DISP_E_UNKNOWNNAME 0x80020006u
#define DISP_E_MEMBERNOTFOUND 0x80020003u
#define CLASS_E_NOAGGREGATION 0x80040110u

static const char *CLSID_WebBrowser = "8856f961-340a-11d0-a96b-00c04fd705a2";

enum { F_UNK, F_OLEOBJ, F_INPLACE, F_ACTIVE, F_VIEW, F_PERSIST, F_CONTROL, F_DISPATCH, F_CPC, F_CP, F_CLASSINFO, F_BROWSER, NFACES };
static const struct { int face; const char *iid; } iids[] = {
    {F_UNK, "00000000-0000-0000-c000-000000000046"},
    {F_OLEOBJ, "00000112-0000-0000-c000-000000000046"},       /* IOleObject */
    {F_INPLACE, "00000114-0000-0000-c000-000000000046"},      /* IOleWindow */
    {F_INPLACE, "00000113-0000-0000-c000-000000000046"},      /* IOleInPlaceObject */
    {F_ACTIVE, "00000117-0000-0000-c000-000000000046"},       /* IOleInPlaceActiveObject */
    {F_VIEW, "0000010d-0000-0000-c000-000000000046"},         /* IViewObject */
    {F_VIEW, "00000127-0000-0000-c000-000000000046"},         /* IViewObject2 */
    {F_PERSIST, "0000010c-0000-0000-c000-000000000046"},      /* IPersist */
    {F_PERSIST, "7fd52380-4e07-101b-ae2d-08002b2ec713"},      /* IPersistStreamInit */
    {F_CONTROL, "b196b288-bab4-101a-b69c-00aa00341d07"},      /* IOleControl */
    {F_DISPATCH, "00020400-0000-0000-c000-000000000046"},     /* IDispatch */
    {F_CPC, "b196b284-bab4-101a-b69c-00aa00341d07"},          /* IConnectionPointContainer */
    {F_CLASSINFO, "b196b283-bab4-101a-b69c-00aa00341d07"},    /* IProvideClassInfo */
    {F_CLASSINFO, "a6bc3ac0-dbaa-11ce-9de3-00aa004bb851"},    /* IProvideClassInfo2 */
    {F_BROWSER, "eab22ac1-30c1-11cf-a7eb-0000c05bae0b"},      /* IWebBrowser */
    {F_BROWSER, "0002df05-0000-0000-c000-000000000046"},      /* IWebBrowserApp */
    {F_BROWSER, "d30c1661-cdaf-11d0-8a3e-00c04fc9e26e"},      /* IWebBrowser2 */
};

typedef struct {
    uint32_t face[NFACES];
    uint32_t refs, site, cx, cy;
    char url[260];
} Browser;

static uint32_t vt[NFACES], vt_factory, factory_obj;

static Browser *self(void) { return (Browser *)com_ptr(rt_arg(0)); }
static void ret(uint32_t hr, int nargs) { rt_ret_stdcall(hr, nargs + 1); }
static void out0(uint32_t p) { if (p) wr32(g_mem, p, 0); }

static uint32_t qi(Browser *b, uint32_t iid, uint32_t out)
{
    for (unsigned i = 0; i < sizeof iids / sizeof *iids; i++)
        if (com_guid_is(iid, iids[i].iid)) {
            b->refs++;
            if (out) wr32(g_mem, out, b->face[iids[i].face]);
            return S_OK;
        }
    char t[40];
    com_guid_text(iid, t);
    fprintf(stderr, "recomp: WebBrowser::QueryInterface(%s): no\n", t);
    out0(out);
    return E_NOINTERFACE;
}

/* ---- comunes ---- */
static void m_qi(void) { ret(qi(self(), rt_arg(1), rt_arg(2)), 2); }
static void m_addref(void) { ret(++self()->refs, 0); }
static void m_release(void) { Browser *b = self(); if (b->refs > 1) b->refs--; ret(b->refs, 0); }
static void ok0(void) { ret(S_OK, 0); }
static void ok1(void) { ret(S_OK, 1); }
static void ok2(void) { ret(S_OK, 2); }
static void ok3(void) { ret(S_OK, 3); }
static void ok4(void) { ret(S_OK, 4); }
static void ok5(void) { ret(S_OK, 5); }
static void ok6(void) { ret(S_OK, 6); }
static void ok10(void) { ret(S_OK, 10); }
static void notimpl1(void) { out0(rt_arg(1)); ret(E_NOTIMPL_, 1); }
static void notimpl2(void) { out0(rt_arg(2)); ret(E_NOTIMPL_, 2); }
static void notimpl3(void) { out0(rt_arg(3)); ret(E_NOTIMPL_, 3); }
static void get0(void) { out0(rt_arg(1)); ret(S_OK, 1); }          /* get_X(*out): 0 */
static void sfalse0(void) { ret(S_FALSE, 0); }

static void put_clsid(uint32_t p)
{
    static const uint8_t g[16] = {0x61, 0xf9, 0x56, 0x88, 0x0a, 0x34, 0xd0, 0x11, 0xa9, 0x6b, 0x00, 0xc0, 0x4f, 0xd7, 0x05, 0xa2};
    if (p) memcpy(g_mem + p, g, 16);
}

/* ---- IOleObject ---- */
static void m_setsite(void) { self()->site = rt_arg(1); ret(S_OK, 1); }
static void m_getsite(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), self()->site); ret(S_OK, 1); }
static void m_getuserclassid(void) { put_clsid(rt_arg(1)); ret(S_OK, 1); }
static void m_setextent(void) { Browser *b = self(); b->cx = rd32(g_mem, rt_arg(2)); b->cy = rd32(g_mem, rt_arg(2) + 4); ret(S_OK, 2); }
static void m_getextent(void) { Browser *b = self(); wr32(g_mem, rt_arg(2), b->cx); wr32(g_mem, rt_arg(2) + 4, b->cy); ret(S_OK, 2); }
static void m_advise2(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 1); ret(S_OK, 2); }   /* Advise(sink, *cookie) */
/* OLEMISC del WebBrowser: SETCLIENTSITEFIRST | ACTIVATEWHENVISIBLE | INSIDEOUT | CANTLINKINSIDE | RECOMPOSEONRESIZE */
static void m_miscstatus(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0x20000u | 0x100u | 0x80u | 0x10u | 0x1u); ret(S_OK, 2); }

/* ---- IOleInPlaceObject (sin ventana propia: GetWindow falla y MFC lo aloja sin HWND) ---- */
static void m_getwindow(void) { out0(rt_arg(1)); ret(0x80004005u, 1); }

/* ---- IViewObject2 ---- */
static void m_getadvise(void) { out0(rt_arg(1)); out0(rt_arg(2)); out0(rt_arg(3)); ret(S_OK, 3); }
static void m_view_extent(void) { Browser *b = self(); wr32(g_mem, rt_arg(4), b->cx); wr32(g_mem, rt_arg(4) + 4, b->cy); ret(S_OK, 4); }

/* ---- IPersistStreamInit ---- */
static void m_getclassid(void) { put_clsid(rt_arg(1)); ret(S_OK, 1); }
static void m_sizemax(void) { if (rt_arg(1)) { wr32(g_mem, rt_arg(1), 0); wr32(g_mem, rt_arg(1) + 4, 0); } ret(S_OK, 1); }

/* ---- IOleControl ---- */
static void m_controlinfo(void)
{
    uint32_t p = rt_arg(1);
    if (p) { wr32(g_mem, p + 4, 0); wr32(g_mem, p + 8, 0); wr32(g_mem, p + 12, 0); }   /* sin aceleradores */
    ret(S_OK, 1);
}

/* ---- IDispatch: sin propiedades ni eventos por nombre ---- */
static void m_typeinfocount(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 0); ret(S_OK, 1); }
static void m_idsofnames(void) { ret(DISP_E_UNKNOWNNAME, 5); }
static void m_invoke(void) { ret(DISP_E_MEMBERNOTFOUND, 8); }

/* ---- IConnectionPointContainer / IConnectionPoint ---- */
static void m_findcp(void) { Browser *b = self(); b->refs++; wr32(g_mem, rt_arg(2), b->face[F_CP]); ret(S_OK, 2); }

/* ---- IWebBrowser2 ---- */
static void note_url(Browser *b, uint32_t bstr)
{
    size_t n = 0;
    for (uint32_t i = 0; bstr && n < sizeof b->url - 1 && rd16(g_mem, bstr + 2 * i); i++) b->url[n++] = (char)rd16(g_mem, bstr + 2 * i);
    b->url[n] = 0;
    fprintf(stderr, "recomp: WebBrowser::Navigate(%s)\n", b->url);
}
static void m_navigate(void) { note_url(self(), rt_arg(1)); ret(S_OK, 5); }
static void m_navigate2(void)                                  /* Navigate2(VARIANT *url, ...): VT_BSTR */
{
    uint32_t v = rt_arg(1);
    if (v && rd16(g_mem, v) == 8) note_url(self(), rd32(g_mem, v + 8));
    ret(S_OK, 5);
}
static void m_readystate(void) { if (rt_arg(1)) wr32(g_mem, rt_arg(1), 4); ret(S_OK, 1); }   /* READYSTATE_COMPLETE */
static void m_querystatus(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), 0); ret(S_OK, 2); }  /* ningún comando habilitado */
static void m_getproperty(void) { if (rt_arg(2)) memset(g_mem + rt_arg(2), 0, 16); ret(S_OK, 2); }
static void m_locationurl(void)
{
    uint32_t oleaut32_bstr_from(const char *s);
    if (rt_arg(1)) wr32(g_mem, rt_arg(1), oleaut32_bstr_from(self()->url));
    ret(S_OK, 1);
}

#define M(n, f) {n, f}
static void build(void)
{
    if (vt[F_UNK]) return;
    const ComMethod unk[3] = {M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release)};
    vt[F_UNK] = com_vtable("WebBrowser:IUnknown", unk, 3);
    const ComMethod ole[24] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("SetClientSite", m_setsite),
        M("GetClientSite", m_getsite), M("SetHostNames", ok2), M("Close", ok1), M("SetMoniker", ok2),
        M("GetMoniker", notimpl3), M("InitFromData", notimpl3), M("GetClipboardData", notimpl2), M("DoVerb", ok6),
        M("EnumVerbs", notimpl1), M("Update", ok0), M("IsUpToDate", ok0), M("GetUserClassID", m_getuserclassid),
        M("GetUserType", notimpl2), M("SetExtent", m_setextent), M("GetExtent", m_getextent), M("Advise", m_advise2),
        M("Unadvise", ok1), M("EnumAdvise", notimpl1), M("GetMiscStatus", m_miscstatus), M("SetColorScheme", ok1)};
    vt[F_OLEOBJ] = com_vtable("WebBrowser:IOleObject", ole, 24);
    const ComMethod inplace[9] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetWindow", m_getwindow),
        M("ContextSensitiveHelp", ok1), M("InPlaceDeactivate", ok0), M("UIDeactivate", ok0), M("SetObjectRects", ok2),
        M("ReactivateAndUndo", ok0)};
    vt[F_INPLACE] = com_vtable("WebBrowser:IOleInPlaceObject", inplace, 9);
    const ComMethod active[10] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetWindow", m_getwindow),
        M("ContextSensitiveHelp", ok1), M("TranslateAccelerator", sfalse0), M("OnFrameWindowActivate", ok1),
        M("OnDocWindowActivate", ok1), M("ResizeBorder", ok3), M("EnableModeless", ok1)};
    vt[F_ACTIVE] = com_vtable("WebBrowser:IOleInPlaceActiveObject", active, 10);
    const ComMethod view[10] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("Draw", ok10),
        M("GetColorSet", notimpl3), M("Freeze", ok4), M("Unfreeze", ok1), M("SetAdvise", ok3), M("GetAdvise", m_getadvise),
        M("GetExtent", m_view_extent)};
    vt[F_VIEW] = com_vtable("WebBrowser:IViewObject2", view, 10);
    const ComMethod persist[9] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetClassID", m_getclassid),
        M("IsDirty", sfalse0), M("Load", ok1), M("Save", ok2), M("GetSizeMax", m_sizemax), M("InitNew", ok0)};
    vt[F_PERSIST] = com_vtable("WebBrowser:IPersistStreamInit", persist, 9);
    const ComMethod control[7] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetControlInfo", m_controlinfo),
        M("OnMnemonic", ok1), M("OnAmbientPropertyChange", ok1), M("FreezeEvents", ok1)};
    vt[F_CONTROL] = com_vtable("WebBrowser:IOleControl", control, 7);
    const ComMethod disp[7] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetTypeInfoCount", m_typeinfocount),
        M("GetTypeInfo", notimpl3), M("GetIDsOfNames", m_idsofnames), M("Invoke", m_invoke)};
    vt[F_DISPATCH] = com_vtable("WebBrowser:IDispatch", disp, 7);
    const ComMethod cpc[5] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("EnumConnectionPoints", notimpl1),
        M("FindConnectionPoint", m_findcp)};
    vt[F_CPC] = com_vtable("WebBrowser:IConnectionPointContainer", cpc, 5);
    const ComMethod cp[8] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetConnectionInterface", notimpl1),
        M("GetConnectionPointContainer", notimpl1), M("Advise", m_advise2), M("Unadvise", ok1), M("EnumConnections", notimpl1)};
    vt[F_CP] = com_vtable("WebBrowser:IConnectionPoint", cp, 8);
    const ComMethod ci[5] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetClassInfo", notimpl1),
        M("GetGUID", notimpl2)};
    vt[F_CLASSINFO] = com_vtable("WebBrowser:IProvideClassInfo2", ci, 5);
    /* IWebBrowser2 (exdisp.h): IDispatch, IWebBrowser (7-31), IWebBrowserApp (32-51), IWebBrowser2 (52-70). */
    const ComMethod br[71] = {
        M("QueryInterface", m_qi), M("AddRef", m_addref), M("Release", m_release), M("GetTypeInfoCount", m_typeinfocount),
        M("GetTypeInfo", notimpl3), M("GetIDsOfNames", m_idsofnames), M("Invoke", m_invoke),
        M("GoBack", ok0), M("GoForward", ok0), M("GoHome", ok0), M("GoSearch", ok0), M("Navigate", m_navigate),
        M("Refresh", ok0), M("Refresh2", ok1), M("Stop", ok0), M("get_Application", get0), M("get_Parent", get0),
        M("get_Container", get0), M("get_Document", get0), M("get_TopLevelContainer", get0), M("get_Type", get0),
        M("get_Left", get0), M("put_Left", ok1), M("get_Top", get0), M("put_Top", ok1), M("get_Width", get0),
        M("put_Width", ok1), M("get_Height", get0), M("put_Height", ok1), M("get_LocationName", get0),
        M("get_LocationURL", m_locationurl), M("get_Busy", get0),
        M("Quit", ok0), M("ClientToWindow", ok2), M("PutProperty", ok5), M("GetProperty", m_getproperty),
        M("get_Name", get0), M("get_HWND", get0), M("get_FullName", get0), M("get_Path", get0), M("get_Visible", get0),
        M("put_Visible", ok1), M("get_StatusBar", get0), M("put_StatusBar", ok1), M("get_StatusText", get0),
        M("put_StatusText", ok1), M("get_ToolBar", get0), M("put_ToolBar", ok1), M("get_MenuBar", get0),
        M("put_MenuBar", ok1), M("get_FullScreen", get0), M("put_FullScreen", ok1),
        M("Navigate2", m_navigate2), M("QueryStatusWB", m_querystatus), M("ExecWB", ok4), M("ShowBrowserBar", ok3),
        M("get_ReadyState", m_readystate), M("get_Offline", get0), M("put_Offline", ok1), M("get_Silent", get0),
        M("put_Silent", ok1), M("get_RegisterAsBrowser", get0), M("put_RegisterAsBrowser", ok1),
        M("get_RegisterAsDropTarget", get0), M("put_RegisterAsDropTarget", ok1), M("get_TheaterMode", get0),
        M("put_TheaterMode", ok1), M("get_AddressBar", get0), M("put_AddressBar", ok1), M("get_Resizable", get0),
        M("put_Resizable", ok1)};
    vt[F_BROWSER] = com_vtable("WebBrowser:IWebBrowser2", br, 71);
}

static uint32_t new_browser(void)
{
    build();
    Browser *b = calloc(1, sizeof *b);
    b->refs = 1;
    for (int i = 0; i < NFACES; i++) b->face[i] = com_new(vt[i], b, 0);
    return b->face[F_UNK];
}

/* ---- la fábrica (IClassFactory2, que también sirve como IClassFactory) ---- */
static void f_qi(void) { if (rt_arg(2)) wr32(g_mem, rt_arg(2), factory_obj); ret(S_OK, 2); }
static void f_addref(void) { ret(2, 0); }
static void f_release(void) { ret(1, 0); }
static void f_create(void)                                     /* CreateInstance(outer, riid, ppv) */
{
    if (rt_arg(1)) { out0(rt_arg(3)); ret(CLASS_E_NOAGGREGATION, 3); return; }
    uint32_t o = new_browser();
    Browser *b = (Browser *)com_ptr(o);
    uint32_t hr = qi(b, rt_arg(2), rt_arg(3));
    b->refs--;                                                 /* el que da el QI queda como única referencia */
    fprintf(stderr, "recomp: WebBrowser creado (%s)\n", hr == S_OK ? "ok" : "interfaz no soportada");
    ret(hr, 3);
}
static void f_licinfo(void)                                    /* GetLicInfo(LICINFO*): no hace falta licencia */
{
    uint32_t p = rt_arg(1);
    if (p) { wr32(g_mem, p, 12); wr32(g_mem, p + 4, 1); wr32(g_mem, p + 8, 1); }
    ret(S_OK, 1);
}
static void f_createlic(void)                                  /* CreateInstanceLic(outer, reserved, riid, key, ppv) */
{
    uint32_t o = new_browser();
    Browser *b = (Browser *)com_ptr(o);
    uint32_t hr = qi(b, rt_arg(3), rt_arg(5));
    b->refs--;
    ret(hr, 5);
}

/* Lo llama CoGetClassObject (com.c): la fábrica del WebBrowser, o 0 si la clase es otra. */
uint32_t webbrowser_factory(uint32_t clsid)
{
    if (!com_guid_is(clsid, CLSID_WebBrowser)) return 0;
    if (!factory_obj) {
        const ComMethod m[8] = {M("QueryInterface", f_qi), M("AddRef", f_addref), M("Release", f_release),
                                M("CreateInstance", f_create), M("LockServer", ok1), M("GetLicInfo", f_licinfo),
                                M("RequestLicKey", notimpl2), M("CreateInstanceLic", f_createlic)};
        vt_factory = com_vtable("WebBrowser:IClassFactory2", m, 8);
        factory_obj = com_new(vt_factory, NULL, 0);
    }
    return factory_obj;
}
