#include <bgfx/bgfx.h>
#include <emscripten.h>
#include <emscripten/em_js.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vs_clear.bin.h"
#include "fs_clear0.bin.h"

EM_JS(void*, harnessImportPageDevice, (), {
	const device = Module.preinitializedWebGPUDevice;
	return device ? WebGPU.importJsDevice(device) : 0;
});

static void hlog(const char* _format, ...)
{
	char buf[2048];
	va_list args;
	va_start(args, _format);
	vsnprintf(buf, sizeof(buf), _format, args);
	va_end(args);
	EM_ASM({ console.log("[h] " + UTF8ToString($0)); }, buf);
}

static int         g_fatalCount = 0;
static int         g_fatalCode  = -1;
static uint32_t    g_fatalFrame = 0;
static char        g_fatalMsg[1024];
static uint32_t    g_frame      = 0;

struct Callback : bgfx::CallbackI
{
	~Callback() override {}

	void fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str) override
	{
		++g_fatalCount;
		g_fatalCode  = int(_code);
		g_fatalFrame = g_frame;
		snprintf(g_fatalMsg, sizeof(g_fatalMsg), "%s", _str);
		hlog("FATAL code=%d (DeviceLost=%d) at %s:%d frame=%u: %s", int(_code), int(bgfx::Fatal::DeviceLost), _filePath, _line, g_frame, _str);
	}

	void traceVargs(const char*, uint16_t, const char* _format, va_list _argList) override
	{
		char buf[2048];
		vsnprintf(buf, sizeof(buf), _format, _argList);
		EM_ASM({ console.log("[bgfx trace] " + UTF8ToString($0)); }, buf);
	}

	void profilerBegin(const char*, uint32_t, const char*, uint16_t) override {}
	void profilerBeginLiteral(const char*, uint32_t, const char*, uint16_t) override {}
	void profilerEnd() override {}
	uint32_t cacheReadSize(uint64_t) override { return 0; }
	bool cacheRead(uint64_t, void*, uint32_t) override { return false; }
	void cacheWrite(uint64_t, const void*, uint32_t) override {}
	void screenShot(const char*, uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, const void*, uint32_t, bool) override {}
	void captureBegin(uint32_t, uint32_t, uint32_t, bgfx::TextureFormat::Enum, bool) override {}
	void captureEnd() override {}
	void captureFrame(const void*, uint32_t) override {}
};

static Callback g_callback;

static const uint16_t kWidth    = 256;
static const uint16_t kHeight   = 256;
static const uint16_t kRbSize   = 16;
static const uint32_t kRbClear  = 0x11223344;
static const uint32_t kSentinel = 0xdeadbeef;

static bool                       g_inited    = false;
static uint32_t                   g_clear     = 0x3366ccff;
static bgfx::TextureHandle        g_rtTex     = BGFX_INVALID_HANDLE;
static bgfx::FrameBufferHandle    g_fb        = BGFX_INVALID_HANDLE;
static bgfx::TextureHandle        g_rbTex     = BGFX_INVALID_HANDLE;
static uint32_t                   g_rbBuf[kRbSize*kRbSize];
static bool                       g_rbRequest = false;
static uint32_t                   g_rbReady   = 0;
static bgfx::ProgramHandle        g_prog      = BGFX_INVALID_HANDLE;
static bgfx::VertexBufferHandle   g_vb        = BGFX_INVALID_HANDLE;
static bgfx::UniformHandle        g_uColor    = BGFX_INVALID_HANDLE;
static bgfx::UniformHandle        g_uDepth    = BGFX_INVALID_HANDLE;
static bgfx::OcclusionQueryHandle g_occ       = BGFX_INVALID_HANDLE;
static bool                       g_occOn     = false;
static int                        g_occResult = -1;
static int32_t                    g_occCount  = -1;
static uint32_t                   g_occVisibleFrames = 0;

static void createResources()
{
	g_rtTex = bgfx::createTexture2D(kRbSize, kRbSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);
	g_fb    = bgfx::createFrameBuffer(1, &g_rtTex, false);
	g_rbTex = bgfx::createTexture2D(kRbSize, kRbSize, false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
	for (uint32_t ii = 0; ii < kRbSize*kRbSize; ++ii) { g_rbBuf[ii] = kSentinel; }

	const bgfx::ShaderHandle vsh = bgfx::createShader(bgfx::makeRef(vs_clear_wgsl, sizeof(vs_clear_wgsl) ) );
	const bgfx::ShaderHandle fsh = bgfx::createShader(bgfx::makeRef(fs_clear0_wgsl, sizeof(fs_clear0_wgsl) ) );
	g_prog   = bgfx::createProgram(vsh, fsh, true);
	g_uColor = bgfx::createUniform("bgfx_clear_color", bgfx::UniformType::Vec4, 8);
	g_uDepth = bgfx::createUniform("bgfx_clear_depth", bgfx::UniformType::Vec4);

	bgfx::VertexLayout layout;
	layout.begin().add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float).end();
	static const float tri[] = { -0.9f, -0.9f, 0.0f,  -0.5f, -0.9f, 0.0f,  -0.9f, -0.5f, 0.0f };
	g_vb  = bgfx::createVertexBuffer(bgfx::makeRef(tri, sizeof(tri) ), layout);
	g_occ = bgfx::createOcclusionQuery();
}

static void destroyResources()
{
	bgfx::destroy(g_occ);
	bgfx::destroy(g_vb);
	bgfx::destroy(g_uDepth);
	bgfx::destroy(g_uColor);
	bgfx::destroy(g_prog);
	bgfx::destroy(g_rbTex);
	bgfx::destroy(g_fb);
	bgfx::destroy(g_rtTex);
}

static void frameBody()
{
	bgfx::setViewRect(0, 0, 0, kWidth, kHeight);
	bgfx::setViewClear(0, BGFX_CLEAR_COLOR | BGFX_CLEAR_DEPTH, g_clear, 1.0f, 0);
	bgfx::touch(0);

	bgfx::setViewFrameBuffer(1, g_fb);
	bgfx::setViewRect(1, 0, 0, kRbSize, kRbSize);
	bgfx::setViewClear(1, BGFX_CLEAR_COLOR, kRbClear, 1.0f, 0);
	bgfx::touch(1);

	if (g_rbRequest)
	{
		g_rbRequest = false;
		bgfx::TextureRegion dst, src, rb;
		dst.init(g_rbTex);
		src.init(g_rtTex);
		rb.init(g_rbTex);
		bgfx::blit(2, dst, src);
		g_rbReady = bgfx::read(rb, g_rbBuf);
		hlog("readTexture requested at frame=%u, ready frame=%u", g_frame, g_rbReady);
	}

	if (g_occOn)
	{
		const float color[4*8] = { 0.0f, 1.0f, 0.0f, 1.0f };
		const float depth[4]   = { 0.0f, 0.0f, 0.0f, 0.0f };
		bgfx::setUniform(g_uColor, color, 8);
		bgfx::setUniform(g_uDepth, depth);
		bgfx::setVertexBuffer(0, g_vb);
		bgfx::setState(BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A);
		bgfx::submit(0, g_prog, g_occ);

		int32_t count = -1;
		g_occResult = int(bgfx::getResult(g_occ, &count) );
		g_occCount  = count;
		if (bgfx::OcclusionQueryResult::Visible == g_occResult) { ++g_occVisibleFrames; }
	}

	g_frame = bgfx::frame();

	EM_ASM({ if (globalThis.__afterFrame) globalThis.__afterFrame(); });
}

static bool initBgfx(bool _pageDevice, bool _profile)
{
	bgfx::Init init;
	init.type     = bgfx::RendererType::WebGPU;
	init.callback = &g_callback;
	init.profile  = _profile;
	init.debug    = false;
	init.swapChain.nwh    = (void*)"#canvas";
	init.swapChain.width  = kWidth;
	init.swapChain.height = kHeight;
	init.reset            = BGFX_RESET_VSYNC;
	init.platformData.context = _pageDevice ? harnessImportPageDevice() : NULL;

	hlog("bgfx::init context=%p profile=%d", init.platformData.context, _profile);

	if (!bgfx::init(init) )
	{
		hlog("bgfx::init failed");
		return false;
	}

	const bgfx::Caps* caps = bgfx::getCaps();
	hlog("bgfx::init ok renderer=%s caps=0x%016llx DRAW_INDIRECT=%d"
		, bgfx::getRendererName(caps->rendererType)
		, (unsigned long long)caps->supported
		, 0 != (caps->supported & BGFX_CAPS_DRAW_INDIRECT)
		);

	if (_profile)
	{
		bgfx::setDebug(BGFX_DEBUG_PROFILER);
	}

	createResources();
	g_inited = true;
	return true;
}

extern "C" {

EMSCRIPTEN_KEEPALIVE int h_init(int _profile) { return initBgfx(true, 0 != _profile) ? 1 : 0; }

EMSCRIPTEN_KEEPALIVE void h_shutdown()
{
	g_inited = false;
	destroyResources();
	bgfx::shutdown();
	hlog("bgfx::shutdown done");
}

EMSCRIPTEN_KEEPALIVE uint32_t h_caps_lo() { return uint32_t(bgfx::getCaps()->supported); }
EMSCRIPTEN_KEEPALIVE uint32_t h_caps_hi() { return uint32_t(bgfx::getCaps()->supported >> 32); }
EMSCRIPTEN_KEEPALIVE int h_draw_indirect() { return 0 != (bgfx::getCaps()->supported & BGFX_CAPS_DRAW_INDIRECT); }
EMSCRIPTEN_KEEPALIVE const char* h_renderer_name() { return g_inited ? bgfx::getRendererName(bgfx::getRendererType() ) : "(not initialized)"; }
EMSCRIPTEN_KEEPALIVE uint32_t h_frame() { return g_frame; }
EMSCRIPTEN_KEEPALIVE int h_fatal_count() { return g_fatalCount; }
EMSCRIPTEN_KEEPALIVE int h_fatal_code() { return g_fatalCode; }
EMSCRIPTEN_KEEPALIVE uint32_t h_fatal_frame() { return g_fatalFrame; }
EMSCRIPTEN_KEEPALIVE const char* h_fatal_msg() { return g_fatalMsg; }
EMSCRIPTEN_KEEPALIVE void h_readback_request() { g_rbRequest = true; }
EMSCRIPTEN_KEEPALIVE uint32_t h_readback_ready() { return g_rbReady; }
EMSCRIPTEN_KEEPALIVE uint32_t h_readback_word(int _idx) { return g_rbBuf[_idx]; }
EMSCRIPTEN_KEEPALIVE void h_occlusion(int _on) { g_occOn = 0 != _on; }
EMSCRIPTEN_KEEPALIVE int h_occ_result() { return g_occResult; }
EMSCRIPTEN_KEEPALIVE int h_occ_count() { return g_occCount; }
EMSCRIPTEN_KEEPALIVE uint32_t h_occ_visible_frames() { return g_occVisibleFrames; }

} // extern "C"

#if HARNESS_ASYNC

static void runFrames(uint32_t _num)
{
	for (uint32_t ii = 0; ii < _num; ++ii)
	{
		frameBody();
		emscripten_sleep(16);
	}
}

int main()
{
	hlog("async main: start");
	EM_ASM({ globalThis.__async = { started: true }; });
	if (!initBgfx(false, false) )
	{
		EM_ASM({ globalThis.__async.init = false; globalThis.__async.done = true; });
		return 0;
	}
	EM_ASM({ globalThis.__async.init = true; globalThis.__async.renderer = UTF8ToString($0); }, bgfx::getRendererName(bgfx::getRendererType() ) );

	runFrames(10);
	g_rbRequest = true;
	EM_ASM({ globalThis.__async.requestedAtFrame = $0; }, g_frame);

	while (0 == g_rbReady || g_frame < g_rbReady)
	{
		runFrames(1);
	}

	hlog("async main: reached ready frame %u (frame=%u) word0=0x%08x", g_rbReady, g_frame, g_rbBuf[0]);
	EM_ASM({
		globalThis.__async.readyFrame = $0;
		globalThis.__async.word0      = $1 >>> 0;
		globalThis.__async.wordLast   = $2 >>> 0;
	}, g_rbReady, g_rbBuf[0], g_rbBuf[kRbSize*kRbSize-1]);

	runFrames(10);

	EM_ASM({ globalThis.__async.frame = $0; globalThis.__async.done = true; }, g_frame);
	hlog("async main: done");
	return 0;
}

#else

static void loop()
{
	if (g_inited)
	{
		frameBody();
	}
}

int main()
{
	emscripten_set_main_loop(loop, 0, false);
	return 0;
}

#endif // HARNESS_ASYNC
