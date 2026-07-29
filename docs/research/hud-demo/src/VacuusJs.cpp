// Minimal QuickJS-ng <-> RmlUi bindings for the VaCuus HUD demo.
// Element handles wrap Rml::ObserverPtr<Rml::Element>, so a handle whose
// element got removed simply goes "dead": methods return false/null.
#include "VacuusJs.h"

#include <RmlUi/Core.h>
#include <quickjs.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace VacuusJs {

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
static JSRuntime* g_rt = nullptr;
static JSContext* g_ctx = nullptr;
static Rml::Context* g_rml = nullptr;
static Rml::ElementDocument* g_doc = nullptr;
static JSClassID g_element_class_id = 0;

static double g_now = 0.0; // seconds, set every frame
static double g_update_ms = 0.0, g_render_ms = 0.0, g_fps = 0.0;
static bool g_exit_requested = false;
static int g_error_count = 0;

struct Timer {
	int64_t id;
	double deadline; // seconds
	double interval; // seconds, <= 0 for one-shot
	JSValue fn;
	bool dead;
};
static std::vector<Timer> g_timers;
static int64_t g_next_timer_id = 1;

static std::vector<JSValue> g_raf_pending; // callbacks for the NEXT frame

// ---------------------------------------------------------------------------
// Error helpers
// ---------------------------------------------------------------------------
static void DumpException()
{
	g_error_count++;
	JSValue exc = JS_GetException(g_ctx);
	const char* msg = JS_ToCString(g_ctx, exc);
	std::fprintf(stderr, "[js-error] %s\n", msg ? msg : "(unknown)");
	if (msg)
		JS_FreeCString(g_ctx, msg);
	JSValue stack = JS_GetPropertyStr(g_ctx, exc, "stack");
	if (!JS_IsUndefined(stack) && !JS_IsException(stack))
	{
		const char* s = JS_ToCString(g_ctx, stack);
		if (s)
		{
			std::fprintf(stderr, "%s\n", s);
			JS_FreeCString(g_ctx, s);
		}
	}
	JS_FreeValue(g_ctx, stack);
	JS_FreeValue(g_ctx, exc);
}

// Call a JS function, dump any exception, free the result.
static void CallVoid(JSValueConst fn, int argc, JSValueConst* argv)
{
	JSValue ret = JS_Call(g_ctx, fn, JS_UNDEFINED, argc, argv);
	if (JS_IsException(ret))
		DumpException();
	JS_FreeValue(g_ctx, ret);
}

static std::string ToStdString(JSValueConst v)
{
	std::string out;
	const char* s = JS_ToCString(g_ctx, v);
	if (s)
	{
		out = s;
		JS_FreeCString(g_ctx, s);
	}
	return out;
}

// ---------------------------------------------------------------------------
// Element handle class
// ---------------------------------------------------------------------------
struct ElementHandle {
	Rml::ObserverPtr<Rml::Element> el;
};

static void ElementFinalizer(JSRuntime* /*rt*/, JSValue val)
{
	delete static_cast<ElementHandle*>(JS_GetOpaque(val, g_element_class_id));
}

static JSClassDef g_element_class_def = {
	"VacuusElement",
	ElementFinalizer,
	nullptr, nullptr, nullptr,
};

static Rml::Element* GetElement(JSValueConst this_val)
{
	auto* handle = static_cast<ElementHandle*>(JS_GetOpaque(this_val, g_element_class_id));
	return handle ? handle->el.get() : nullptr;
}

static JSValue WrapElement(Rml::Element* el)
{
	if (!el)
		return JS_NULL;
	JSValue obj = JS_NewObjectClass(g_ctx, g_element_class_id);
	JS_SetOpaque(obj, new ElementHandle{el->GetObserverPtr()});
	return obj;
}

// --- JS event listener -------------------------------------------------------
// Self-deletes when detached from its element (element removed/destroyed).
class JsEventListener;
static std::unordered_set<JsEventListener*> g_listeners;

class JsEventListener final : public Rml::EventListener {
public:
	JsEventListener(JSValue fn) : fn_(fn) { g_listeners.insert(this); }
	~JsEventListener() override { g_listeners.erase(this); }

	void ProcessEvent(Rml::Event& event) override
	{
		if (!g_ctx)
			return;
		JSValue ev = JS_NewObject(g_ctx);
		JS_SetPropertyStr(g_ctx, ev, "type", JS_NewString(g_ctx, event.GetType().c_str()));
		Rml::Element* target = event.GetTargetElement();
		JS_SetPropertyStr(g_ctx, ev, "targetId", JS_NewString(g_ctx, target ? target->GetId().c_str() : ""));
		JS_SetPropertyStr(g_ctx, ev, "target", WrapElement(target));
		JSValueConst argv[1] = {ev};
		CallVoid(fn_, 1, argv);
		JS_FreeValue(g_ctx, ev);
	}

	void OnDetach(Rml::Element* /*element*/) override
	{
		if (g_ctx)
			JS_FreeValue(g_ctx, fn_);
		delete this;
	}

	void FreeAtShutdown()
	{
		// JS context is going away; element tree is already gone.
		JS_FreeValue(g_ctx, fn_);
	}

private:
	JSValue fn_;
};

// ---------------------------------------------------------------------------
// Element methods
// ---------------------------------------------------------------------------
static JSValue el_isValid(JSContext*, JSValueConst this_val, int, JSValueConst*)
{
	return JS_NewBool(g_ctx, GetElement(this_val) != nullptr);
}

static JSValue el_setInnerRML(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 1)
		return JS_FALSE;
	el->SetInnerRML(ToStdString(argv[0]));
	return JS_TRUE;
}

static JSValue el_setAttribute(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 2)
		return JS_FALSE;
	el->SetAttribute(ToStdString(argv[0]), ToStdString(argv[1]));
	return JS_TRUE;
}

static JSValue el_getAttribute(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 1)
		return JS_NULL;
	Rml::Variant* attr = el->GetAttribute(ToStdString(argv[0]));
	if (!attr)
		return JS_NULL;
	return JS_NewString(g_ctx, attr->Get<Rml::String>().c_str());
}

static JSValue el_setProperty(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 2)
		return JS_FALSE;
	return JS_NewBool(g_ctx, el->SetProperty(ToStdString(argv[0]), ToStdString(argv[1])));
}

static JSValue el_removeProperty(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 1)
		return JS_FALSE;
	el->RemoveProperty(ToStdString(argv[0]));
	return JS_TRUE;
}

static JSValue el_setClass(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 2)
		return JS_FALSE;
	el->SetClass(ToStdString(argv[0]), JS_ToBool(g_ctx, argv[1]) != 0);
	return JS_TRUE;
}

// Parse an RML string and append the resulting element(s) as children.
// Returns the handle of the last appended element, or null.
static JSValue el_appendRML(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 1 || !g_doc)
		return JS_NULL;
	Rml::ElementPtr temp = g_doc->CreateElement("div");
	temp->SetInnerRML(ToStdString(argv[0]));
	Rml::Element* last = nullptr;
	while (temp->GetNumChildren() > 0)
	{
		Rml::ElementPtr child = temp->RemoveChild(temp->GetChild(0));
		last = el->AppendChild(std::move(child));
	}
	return WrapElement(last);
}

static JSValue el_remove(JSContext*, JSValueConst this_val, int, JSValueConst*)
{
	Rml::Element* el = GetElement(this_val);
	if (!el)
		return JS_FALSE;
	Rml::Element* parent = el->GetParentNode();
	if (!parent)
		return JS_FALSE;
	parent->RemoveChild(el); // returned ElementPtr destroys the element
	return JS_TRUE;
}

static JSValue el_addEventListener(JSContext*, JSValueConst this_val, int argc, JSValueConst* argv)
{
	Rml::Element* el = GetElement(this_val);
	if (!el || argc < 2 || !JS_IsFunction(g_ctx, argv[1]))
		return JS_FALSE;
	auto* listener = new JsEventListener(JS_DupValue(g_ctx, argv[1]));
	el->AddEventListener(ToStdString(argv[0]), listener);
	return JS_TRUE;
}

static const JSCFunctionListEntry g_element_proto_funcs[] = {
	JS_CFUNC_DEF("isValid", 0, el_isValid),
	JS_CFUNC_DEF("setInnerRML", 1, el_setInnerRML),
	JS_CFUNC_DEF("setAttribute", 2, el_setAttribute),
	JS_CFUNC_DEF("getAttribute", 1, el_getAttribute),
	JS_CFUNC_DEF("setProperty", 2, el_setProperty),
	JS_CFUNC_DEF("removeProperty", 1, el_removeProperty),
	JS_CFUNC_DEF("setClass", 2, el_setClass),
	JS_CFUNC_DEF("appendRML", 1, el_appendRML),
	JS_CFUNC_DEF("remove", 0, el_remove),
	JS_CFUNC_DEF("addEventListener", 2, el_addEventListener),
};

// ---------------------------------------------------------------------------
// vacuus.* functions
// ---------------------------------------------------------------------------
static JSValue vc_getElementById(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	if (argc < 1 || !g_doc)
		return JS_NULL;
	return WrapElement(g_doc->GetElementById(ToStdString(argv[0])));
}

// createElementIn(parentId, tag [, innerRML]) -> element handle | null
static JSValue vc_createElementIn(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	if (argc < 2 || !g_doc)
		return JS_NULL;
	Rml::Element* parent = g_doc->GetElementById(ToStdString(argv[0]));
	if (!parent)
		return JS_NULL;
	Rml::ElementPtr el = g_doc->CreateElement(ToStdString(argv[1]));
	if (!el)
		return JS_NULL;
	if (argc >= 3 && !JS_IsUndefined(argv[2]) && !JS_IsNull(argv[2]))
		el->SetInnerRML(ToStdString(argv[2]));
	return WrapElement(parent->AppendChild(std::move(el)));
}

static JSValue vc_stats(JSContext*, JSValueConst, int, JSValueConst*)
{
	JSValue obj = JS_NewObject(g_ctx);
	JS_SetPropertyStr(g_ctx, obj, "updateMs", JS_NewFloat64(g_ctx, g_update_ms));
	JS_SetPropertyStr(g_ctx, obj, "renderMs", JS_NewFloat64(g_ctx, g_render_ms));
	JS_SetPropertyStr(g_ctx, obj, "fps", JS_NewFloat64(g_ctx, g_fps));
	return obj;
}

static JSValue vc_contextSize(JSContext*, JSValueConst, int, JSValueConst*)
{
	JSValue obj = JS_NewObject(g_ctx);
	Rml::Vector2i dim = g_rml ? g_rml->GetDimensions() : Rml::Vector2i(0, 0);
	JS_SetPropertyStr(g_ctx, obj, "w", JS_NewInt32(g_ctx, dim.x));
	JS_SetPropertyStr(g_ctx, obj, "h", JS_NewInt32(g_ctx, dim.y));
	return obj;
}

static JSValue PrintArgs(const char* prefix, int argc, JSValueConst* argv, FILE* stream)
{
	std::fputs(prefix, stream);
	for (int i = 0; i < argc; i++)
	{
		const char* s = JS_ToCString(g_ctx, argv[i]);
		std::fprintf(stream, "%s%s", i ? " " : "", s ? s : "(null)");
		if (s)
			JS_FreeCString(g_ctx, s);
	}
	std::fputc('\n', stream);
	std::fflush(stream);
	return JS_UNDEFINED;
}

static JSValue vc_log(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	return PrintArgs("[vacuus] ", argc, argv, stdout);
}

static JSValue vc_exit(JSContext*, JSValueConst, int, JSValueConst*)
{
	g_exit_requested = true;
	return JS_UNDEFINED;
}

static const JSCFunctionListEntry g_vacuus_funcs[] = {
	JS_CFUNC_DEF("getElementById", 1, vc_getElementById),
	JS_CFUNC_DEF("createElementIn", 3, vc_createElementIn),
	JS_CFUNC_DEF("stats", 0, vc_stats),
	JS_CFUNC_DEF("contextSize", 0, vc_contextSize),
	JS_CFUNC_DEF("log", 1, vc_log),
	JS_CFUNC_DEF("exit", 0, vc_exit),
};

// ---------------------------------------------------------------------------
// console.*
// ---------------------------------------------------------------------------
static JSValue console_log(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	return PrintArgs("[js] ", argc, argv, stdout);
}
static JSValue console_warn(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	return PrintArgs("[js-warn] ", argc, argv, stderr);
}
static JSValue console_error(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	return PrintArgs("[js-err] ", argc, argv, stderr);
}

// ---------------------------------------------------------------------------
// Timers + requestAnimationFrame
// ---------------------------------------------------------------------------
static JSValue AddTimer(int argc, JSValueConst* argv, bool repeat)
{
	if (argc < 1 || !JS_IsFunction(g_ctx, argv[0]))
		return JS_ThrowTypeError(g_ctx, "expected a function");
	double ms = 0;
	if (argc >= 2)
		JS_ToFloat64(g_ctx, &ms, argv[1]);
	if (ms < 0)
		ms = 0;
	Timer t;
	t.id = g_next_timer_id++;
	t.interval = repeat ? (ms / 1000.0) : 0.0;
	t.deadline = g_now + ms / 1000.0;
	t.fn = JS_DupValue(g_ctx, argv[0]);
	t.dead = false;
	g_timers.push_back(t);
	return JS_NewInt64(g_ctx, t.id);
}

static JSValue js_setTimeout(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	return AddTimer(argc, argv, false);
}
static JSValue js_setInterval(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	return AddTimer(argc, argv, true);
}
static JSValue js_clearTimer(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	if (argc >= 1)
	{
		int64_t id = 0;
		JS_ToInt64(g_ctx, &id, argv[0]);
		for (Timer& t : g_timers)
			if (t.id == id)
				t.dead = true;
	}
	return JS_UNDEFINED;
}

static JSValue js_requestAnimationFrame(JSContext*, JSValueConst, int argc, JSValueConst* argv)
{
	if (argc < 1 || !JS_IsFunction(g_ctx, argv[0]))
		return JS_ThrowTypeError(g_ctx, "expected a function");
	g_raf_pending.push_back(JS_DupValue(g_ctx, argv[0]));
	return JS_UNDEFINED;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
bool Initialize(Rml::Context* rml_context, Rml::ElementDocument* document, double auto_exit_seconds)
{
	g_rml = rml_context;
	g_doc = document;

	g_rt = JS_NewRuntime();
	if (!g_rt)
		return false;
	g_ctx = JS_NewContext(g_rt);
	if (!g_ctx)
		return false;

	JS_NewClassID(g_rt, &g_element_class_id);
	JS_NewClass(g_rt, g_element_class_id, &g_element_class_def);
	JSValue proto = JS_NewObject(g_ctx);
	JS_SetPropertyFunctionList(g_ctx, proto, g_element_proto_funcs,
		sizeof(g_element_proto_funcs) / sizeof(g_element_proto_funcs[0]));
	JS_SetClassProto(g_ctx, g_element_class_id, proto);

	JSValue global = JS_GetGlobalObject(g_ctx);

	JSValue vacuus = JS_NewObject(g_ctx);
	JS_SetPropertyFunctionList(g_ctx, vacuus, g_vacuus_funcs,
		sizeof(g_vacuus_funcs) / sizeof(g_vacuus_funcs[0]));
	JS_SetPropertyStr(g_ctx, global, "vacuus", vacuus);

	JSValue console = JS_NewObject(g_ctx);
	JS_SetPropertyStr(g_ctx, console, "log", JS_NewCFunction(g_ctx, console_log, "log", 1));
	JS_SetPropertyStr(g_ctx, console, "warn", JS_NewCFunction(g_ctx, console_warn, "warn", 1));
	JS_SetPropertyStr(g_ctx, console, "error", JS_NewCFunction(g_ctx, console_error, "error", 1));
	JS_SetPropertyStr(g_ctx, global, "console", console);

	JS_SetPropertyStr(g_ctx, global, "setTimeout", JS_NewCFunction(g_ctx, js_setTimeout, "setTimeout", 2));
	JS_SetPropertyStr(g_ctx, global, "setInterval", JS_NewCFunction(g_ctx, js_setInterval, "setInterval", 2));
	JS_SetPropertyStr(g_ctx, global, "clearTimeout", JS_NewCFunction(g_ctx, js_clearTimer, "clearTimeout", 1));
	JS_SetPropertyStr(g_ctx, global, "clearInterval", JS_NewCFunction(g_ctx, js_clearTimer, "clearInterval", 1));
	JS_SetPropertyStr(g_ctx, global, "requestAnimationFrame",
		JS_NewCFunction(g_ctx, js_requestAnimationFrame, "requestAnimationFrame", 1));

	JS_SetPropertyStr(g_ctx, global, "AUTO_EXIT_SECONDS", JS_NewFloat64(g_ctx, auto_exit_seconds));

	JS_FreeValue(g_ctx, global);
	return true;
}

void Shutdown()
{
	if (!g_ctx)
		return;
	for (Timer& t : g_timers)
		JS_FreeValue(g_ctx, t.fn);
	g_timers.clear();
	for (JSValue& v : g_raf_pending)
		JS_FreeValue(g_ctx, v);
	g_raf_pending.clear();
	// Listeners still alive here were never detached (should be none if the
	// document tree was destroyed first). Free their JS functions anyway.
	for (JsEventListener* l : std::unordered_set<JsEventListener*>(g_listeners))
	{
		l->FreeAtShutdown();
		delete l;
	}
	g_listeners.clear();

	JS_FreeContext(g_ctx);
	JS_FreeRuntime(g_rt);
	g_ctx = nullptr;
	g_rt = nullptr;
	g_rml = nullptr;
	g_doc = nullptr;
}

bool EvalFile(const char* path)
{
	FILE* f = std::fopen(path, "rb");
	if (!f)
	{
		std::fprintf(stderr, "[vacuus] cannot open JS file: %s\n", path);
		return false;
	}
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	std::string src(size_t(size), '\0');
	size_t read = std::fread(&src[0], 1, size_t(size), f);
	std::fclose(f);
	src.resize(read);

	JSValue ret = JS_Eval(g_ctx, src.c_str(), src.size(), path, JS_EVAL_TYPE_GLOBAL);
	bool ok = !JS_IsException(ret);
	if (!ok)
		DumpException();
	JS_FreeValue(g_ctx, ret);
	return ok;
}

void OnFrame(double now_seconds)
{
	if (!g_ctx)
		return;
	g_now = now_seconds;

	// requestAnimationFrame: run callbacks queued for this frame. Callbacks
	// queued while running go into the fresh g_raf_pending for next frame.
	std::vector<JSValue> raf_now;
	raf_now.swap(g_raf_pending);
	JSValue ts = JS_NewFloat64(g_ctx, now_seconds * 1000.0);
	for (JSValue& fn : raf_now)
	{
		JSValueConst argv[1] = {ts};
		CallVoid(fn, 1, argv);
		JS_FreeValue(g_ctx, fn);
	}
	JS_FreeValue(g_ctx, ts);

	// Timers. Index loop: callbacks may append new timers.
	for (size_t i = 0; i < g_timers.size(); i++)
	{
		if (g_timers[i].dead || g_timers[i].deadline > g_now)
			continue;
		if (g_timers[i].interval > 0.0)
			g_timers[i].deadline = g_now + g_timers[i].interval;
		else
			g_timers[i].dead = true;
		JSValue fn = JS_DupValue(g_ctx, g_timers[i].fn); // callback may clear itself
		CallVoid(fn, 0, nullptr);
		JS_FreeValue(g_ctx, fn);
	}
	for (size_t i = 0; i < g_timers.size();)
	{
		if (g_timers[i].dead)
		{
			JS_FreeValue(g_ctx, g_timers[i].fn);
			g_timers.erase(g_timers.begin() + long(i));
		}
		else
			i++;
	}

	// Promise jobs etc.
	for (;;)
	{
		JSContext* job_ctx = nullptr;
		int r = JS_ExecutePendingJob(g_rt, &job_ctx);
		if (r <= 0)
		{
			if (r < 0)
				DumpException();
			break;
		}
	}
}

void OnKey(const char* key_name)
{
	if (!g_ctx)
		return;
	JSValue global = JS_GetGlobalObject(g_ctx);
	JSValue vacuus = JS_GetPropertyStr(g_ctx, global, "vacuus");
	JSValue fn = JS_GetPropertyStr(g_ctx, vacuus, "onKey");
	if (JS_IsFunction(g_ctx, fn))
	{
		JSValue key = JS_NewString(g_ctx, key_name);
		JSValueConst argv[1] = {key};
		CallVoid(fn, 1, argv);
		JS_FreeValue(g_ctx, key);
	}
	JS_FreeValue(g_ctx, fn);
	JS_FreeValue(g_ctx, vacuus);
	JS_FreeValue(g_ctx, global);
}

void SetStats(double update_ms, double render_ms, double fps)
{
	g_update_ms = update_ms;
	g_render_ms = render_ms;
	g_fps = fps;
}

bool ExitRequested()
{
	return g_exit_requested;
}

int ErrorCount()
{
	return g_error_count;
}

} // namespace VacuusJs
