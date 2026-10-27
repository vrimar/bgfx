#include <bgfx/bgfx.h>
#include <webgpu/webgpu.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

static std::thread::id s_mainThread;
static std::atomic<uint64_t> s_progress{0};
static bool s_verbose = false;

static const char* threadName(std::thread::id _id)
{
	return _id == s_mainThread ? "app(main) thread" : "OTHER thread";
}

struct Callback : public bgfx::CallbackI
{
	std::atomic<int> deviceLost{0};
	std::atomic<int> debugCheck{0};

	void fatal(const char* _filePath, uint16_t _line, bgfx::Fatal::Enum _code, const char* _str) override
	{
		fprintf(stderr, "[bgfx fatal] code=%d %s (%s:%d)\n", _code, _str, _filePath, _line);
		if (bgfx::Fatal::DeviceLost == _code) { ++deviceLost; return; }
		if (bgfx::Fatal::DebugCheck == _code) { ++debugCheck; return; }
		_exit(3);
	}

	void traceVargs(const char* _filePath, uint16_t _line, const char* _format, va_list _argList) override
	{
		char buf[2048];
		vsnprintf(buf, sizeof(buf), _format, _argList);
		if (s_verbose
		||  strstr(buf, "lost") || strstr(buf, "Lost") || strstr(buf, "readback") || strstr(buf, "map failed")
		||  strstr(buf, "uncaptured") || strstr(buf, "Init error") || strstr(buf, "errorState") )
		{
			fprintf(stderr, "[bgfx trace] %s:%d %s", _filePath, _line, buf);
		}
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

static Callback s_callback;

static WGPUInstance s_instance;
static WGPUAdapter  s_adapter;
static WGPUDevice   s_device;

static bool             s_appIndirect = false;
static std::atomic<int> s_appLost{0};
static std::thread::id  s_appLostThread;
static std::atomic<int> s_appErrors{0};

static WGPUStringView sv(const char* _str) { return WGPUStringView{ _str, strlen(_str) }; }

static bool createAppDevice(bool _indirect)
{
	WGPUInstanceFeatureName instanceFeatures[] = { WGPUInstanceFeatureName_TimedWaitAny };
	WGPUInstanceDescriptor instanceDesc = WGPU_INSTANCE_DESCRIPTOR_INIT;
	instanceDesc.requiredFeatureCount = 1;
	instanceDesc.requiredFeatures     = instanceFeatures;
	s_instance = wgpuCreateInstance(&instanceDesc);

	WGPURequestAdapterOptions rao = WGPU_REQUEST_ADAPTER_OPTIONS_INIT;
	rao.powerPreference = WGPUPowerPreference_HighPerformance;
	rao.backendType     = WGPUBackendType_Vulkan;

	WGPURequestAdapterCallbackInfo adapterCb = WGPU_REQUEST_ADAPTER_CALLBACK_INFO_INIT;
	adapterCb.mode     = WGPUCallbackMode_WaitAnyOnly;
	adapterCb.callback = [](WGPURequestAdapterStatus, WGPUAdapter _adapter, WGPUStringView, void* _ud, void*) { *(WGPUAdapter*)_ud = _adapter; };
	adapterCb.userdata1 = &s_adapter;
	WGPUFutureWaitInfo fwi = { wgpuInstanceRequestAdapter(s_instance, &rao, adapterCb), false };
	wgpuInstanceWaitAny(s_instance, 1, &fwi, UINT64_MAX);
	if (NULL == s_adapter) { fprintf(stderr, "no adapter\n"); return false; }

	WGPUFeatureName features[8];
	uint32_t numFeatures = 0;
	const WGPUFeatureName wanted[] =
	{
		WGPUFeatureName_TimestampQuery,
		WGPUFeatureName_ImplicitDeviceSynchronization,
		WGPUFeatureName_IndirectFirstInstance,
	};
	for (WGPUFeatureName f : wanted)
	{
		if (!_indirect && WGPUFeatureName_IndirectFirstInstance == f) continue;
		if (wgpuAdapterHasFeature(s_adapter, f) )
		{
			features[numFeatures++] = f;
			s_appIndirect |= WGPUFeatureName_IndirectFirstInstance == f;
		}
	}

	const char* toggles[] = { "allow_unsafe_apis" };
	WGPUDawnTogglesDescriptor togglesDesc = WGPU_DAWN_TOGGLES_DESCRIPTOR_INIT;
	togglesDesc.enabledToggleCount = 1;
	togglesDesc.enabledToggles     = toggles;

	WGPUDeviceDescriptor deviceDesc = WGPU_DEVICE_DESCRIPTOR_INIT;
	deviceDesc.nextInChain          = &togglesDesc.chain;
	deviceDesc.requiredFeatureCount = numFeatures;
	deviceDesc.requiredFeatures     = features;
	deviceDesc.deviceLostCallbackInfo.mode     = WGPUCallbackMode_AllowSpontaneous;
	deviceDesc.deviceLostCallbackInfo.callback = [](const WGPUDevice*, WGPUDeviceLostReason _reason, WGPUStringView _msg, void*, void*)
	{
		s_appLostThread = std::this_thread::get_id();
		++s_appLost;
		fprintf(stderr, "[app] device lost callback reason=%d on %s: %.*s\n", _reason, threadName(s_appLostThread), int(_msg.length == WGPU_STRLEN ? 0 : _msg.length), _msg.data);
	};
	deviceDesc.uncapturedErrorCallbackInfo.callback = [](const WGPUDevice*, WGPUErrorType _type, WGPUStringView _msg, void*, void*)
	{
		if (++s_appErrors <= 3) fprintf(stderr, "[app] uncaptured error %d: %.*s\n", _type, int(_msg.length == WGPU_STRLEN ? 0 : _msg.length), _msg.data);
	};

	WGPURequestDeviceCallbackInfo deviceCb = WGPU_REQUEST_DEVICE_CALLBACK_INFO_INIT;
	deviceCb.mode     = WGPUCallbackMode_WaitAnyOnly;
	deviceCb.callback = [](WGPURequestDeviceStatus, WGPUDevice _device, WGPUStringView _msg, void* _ud, void*) { *(WGPUDevice*)_ud = _device; if (NULL == _device) fprintf(stderr, "device: %.*s\n", int(_msg.length), _msg.data); };
	deviceCb.userdata1 = &s_device;
	fwi = { wgpuAdapterRequestDevice(s_adapter, &deviceDesc, deviceCb), false };
	wgpuInstanceWaitAny(s_instance, 1, &fwi, UINT64_MAX);
	if (NULL == s_device) { fprintf(stderr, "no device\n"); return false; }

	fprintf(stderr, "[app] device features:");
	for (uint32_t ii = 0; ii < numFeatures; ++ii) fprintf(stderr, " %x", features[ii]);
	fprintf(stderr, "\n");
	return true;
}

struct Args
{
	std::string mode;
	bool own = false;
	bool indirect = true;
	uint32_t frames = 200;
	uint32_t views = 8;
	uint32_t size = 256;
};

static bgfx::FrameBufferHandle s_fb = BGFX_INVALID_HANDLE;

static void renderFrame(const Args& _args)
{
	for (uint16_t vv = 0; vv < _args.views; ++vv)
	{
		bgfx::setViewFrameBuffer(vv, s_fb);
		bgfx::setViewRect(vv, 0, 0, uint16_t(_args.size), uint16_t(_args.size) );
		bgfx::setViewClear(vv, BGFX_CLEAR_COLOR, 0x336699ff + vv, 1.0f, 0);
		bgfx::touch(vv);
	}
	bgfx::frame();
	++s_progress;
}

static void startWatchdog(uint32_t _seconds)
{
	std::thread([_seconds]
	{
		uint64_t last = s_progress;
		uint32_t still = 0;
		for (;;)
		{
			std::this_thread::sleep_for(std::chrono::seconds(1) );
			const uint64_t now = s_progress;
			still = now == last ? still + 1 : 0;
			last = now;
			if (still >= _seconds)
			{
				fprintf(stdout, "RESULT BUG hang: no frame progress for %us at frame %llu\n", _seconds, (unsigned long long)now);
				fflush(stdout);
				_exit(2);
			}
		}
	}).detach();
}

static double cpuSeconds()
{
	rusage ru;
	getrusage(RUSAGE_SELF, &ru);
	return ru.ru_utime.tv_sec + ru.ru_stime.tv_sec + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec)*1e-6;
}

int main(int _argc, const char* _argv[])
{
	s_mainThread = std::this_thread::get_id();

	Args args;
	args.mode = _argc > 1 ? _argv[1] : "caps";
	for (int ii = 2; ii < _argc; ++ii)
	{
		if (0 == strcmp(_argv[ii], "--own") ) args.own = true;
		else if (0 == strcmp(_argv[ii], "--no-indirect") ) args.indirect = false;
		else if (0 == strcmp(_argv[ii], "--frames") ) args.frames = atoi(_argv[++ii]);
		else if (0 == strcmp(_argv[ii], "--views") ) args.views = atoi(_argv[++ii]);
		else if (0 == strcmp(_argv[ii], "--size") ) args.size = atoi(_argv[++ii]);
		else if (0 == strcmp(_argv[ii], "-v") ) s_verbose = true;
	}

	if (!args.own && !createAppDevice(args.indirect) )
	{
		return 1;
	}

	bgfx::Init init;
	init.type = bgfx::RendererType::WebGPU;
	init.swapChain.width  = args.own ? 0 : args.size;
	init.swapChain.height = args.own ? 0 : args.size;
	init.callback = &s_callback;
	init.profile  = true;
	if (!args.own)
	{
		init.platformData.context = s_device;
	}

	startWatchdog(15);

	if (!bgfx::init(init) )
	{
		printf("RESULT BUG init failed\n");
		return 1;
	}

	const bgfx::Caps* caps = bgfx::getCaps();
	printf("INFO renderer=%s external=%d drawIndirect=%d vendor=%x\n"
		, bgfx::getRendererName(caps->rendererType)
		, !args.own
		, 0 != (caps->supported & BGFX_CAPS_DRAW_INDIRECT)
		, caps->vendorId
		);

	s_fb = bgfx::createFrameBuffer(uint16_t(args.size), uint16_t(args.size), bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_RT);

	if ("caps" == args.mode)
	{
		renderFrame(args);
		const bool drawIndirect = 0 != (caps->supported & BGFX_CAPS_DRAW_INDIRECT);
		if (args.own)
		{
			printf("RESULT INFO drawIndirect=%d\n", drawIndirect);
		}
		else
		{
			printf("RESULT %s drawIndirect=%d, device IndirectFirstInstance=%d\n", drawIndirect == s_appIndirect ? "OK" : "BUG", drawIndirect, s_appIndirect);
		}
	}
	else if ("appcb" == args.mode)
	{
		WGPUBufferDescriptor bd = WGPU_BUFFER_DESCRIPTOR_INIT;
		bd.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
		bd.size  = 256;
		WGPUBuffer buffer = wgpuDeviceCreateBuffer(s_device, &bd);

		static std::atomic<int> fired{0};
		static std::thread::id firedOn;
		WGPUBufferMapCallbackInfo cbi = WGPU_BUFFER_MAP_CALLBACK_INFO_INIT;
		cbi.mode     = WGPUCallbackMode_AllowProcessEvents;
		cbi.callback = [](WGPUMapAsyncStatus, WGPUStringView, void*, void*) { firedOn = std::this_thread::get_id(); ++fired; };
		wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, 256, cbi);

		for (uint32_t ii = 0; ii < args.frames; ++ii) renderFrame(args);
		bgfx::frame();
		bgfx::frame();

		if (0 != fired)
		{
			printf("RESULT BUG app callback ran inside bgfx, on %s, before the app pumped\n", threadName(firedOn) );
		}
		else
		{
			wgpuInstanceProcessEvents(s_instance);
			const bool ok = 1 == fired && firedOn == s_mainThread;
			printf("RESULT %s app callback waited for the app's ProcessEvents; then fired=%d on %s\n", ok ? "OK" : "BUG", fired.load(), fired ? threadName(firedOn) : "-");
		}
		wgpuBufferRelease(buffer);
	}
	else if ("pump" == args.mode)
	{
		std::atomic<bool> stop{false};
		std::atomic<uint64_t> pumps{0};
		std::thread pumper([&]
		{
			while (!stop) { wgpuInstanceProcessEvents(s_instance); ++pumps; }
		});

		const auto t0 = std::chrono::steady_clock::now();
		for (uint32_t ii = 0; ii < args.frames; ++ii) renderFrame(args);
		const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		stop = true;
		pumper.join();
		printf("RESULT OK completed %u frames x %u views in %.2fs while the app pumped %llu times\n", args.frames, args.views, secs, (unsigned long long)pumps.load() );
	}
	else if ("lost" == args.mode)
	{
		for (uint32_t ii = 0; ii < 20; ++ii) renderFrame(args);

		wgpuDeviceForceLoss(s_device, WGPUDeviceLostReason_Unknown, sv("repro forced loss") );

		for (uint32_t ii = 0; ii < args.frames; ++ii) renderFrame(args);

		const bool ok = 1 == s_callback.deviceLost && bgfx::RendererType::Noop == bgfx::getRendererType();
		printf("RESULT %s after forced loss: bgfx DeviceLost fatals=%d renderer=%s app lost callbacks=%d (on %s) frames=%llu\n"
			, ok ? "OK" : "BUG"
			, s_callback.deviceLost.load()
			, bgfx::getRendererName(bgfx::getRendererType() )
			, s_appLost.load()
			, s_appLost ? threadName(s_appLostThread) : "-"
			, (unsigned long long)s_progress.load()
			);
	}
	else if ("readback" == args.mode)
	{
		bgfx::TextureHandle rb = bgfx::createTexture2D(uint16_t(args.size), uint16_t(args.size), false, 1, bgfx::TextureFormat::RGBA8, BGFX_TEXTURE_BLIT_DST | BGFX_TEXTURE_READ_BACK);
		std::string pixels(args.size*args.size*4, '\0');

		Args one = args;
		one.views = 1;
		renderFrame(one);
		bgfx::TextureRegion dst, src, whole;
		dst.init(rb);
		src.init(bgfx::getTexture(s_fb) );
		whole.init(rb);
		bgfx::blit(1, dst, src);
		bgfx::touch(1);
		const uint32_t ready = bgfx::read(whole, pixels.data() );
		uint32_t frame = 0;
		for (uint32_t ii = 0; ii < 10 && frame < ready; ++ii) { frame = bgfx::frame(); ++s_progress; }

		const uint8_t* p = (const uint8_t*)pixels.data();
		const bool ok = 0x33 == p[0] && 0x66 == p[1] && 0x99 == p[2] && 0xff == p[3];
		printf("RESULT %s readback ready=%u frame=%u pixel0=%02x%02x%02x%02x (expect 336699ff)\n", ok ? "OK" : "BUG", ready, frame, p[0], p[1], p[2], p[3]);
		bgfx::destroy(rb);
	}
	else if ("cpu" == args.mode)
	{
		for (uint32_t ii = 0; ii < 10; ++ii) renderFrame(args);
		const double c0 = cpuSeconds();
		const auto t0 = std::chrono::steady_clock::now();
		for (uint32_t ii = 0; ii < args.frames; ++ii) renderFrame(args);
		const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		const double cpu = cpuSeconds() - c0;
		const bgfx::Stats* stats = bgfx::getStats();
		const double gpuMs = stats->gpuTimerFreq ? double(stats->gpuTimeEnd - stats->gpuTimeBegin)*1000.0/stats->gpuTimerFreq : -1.0;
		const bool gpuBound = gpuMs*args.frames >= 0.8*wall*1000.0;
		printf("RESULT %s %u frames: wall %.2fs, process cpu %.2fs (%.0f%% of one core), last gpu frame %.2f ms%s\n"
			, !gpuBound ? "INFO" : cpu < 0.5*wall ? "OK" : "BUG"
			, args.frames, wall, cpu, 100.0*cpu/wall, gpuMs
			, gpuBound ? "" : " (not GPU-bound, raise --size)"
			);
	}

	bgfx::destroy(s_fb);
	bgfx::shutdown();

	printf("INFO bgfx debug checks=%d app uncaptured errors=%d\n", s_callback.debugCheck.load(), s_appErrors.load() );
	fflush(stdout);
	_exit(0);
}
