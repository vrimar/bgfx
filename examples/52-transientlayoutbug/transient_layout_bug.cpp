/*
 * Copyright 2011-2026 Branimir Karadzic. All rights reserved.
 * License: https://github.com/bkaradzic/bgfx/blob/master/LICENSE
 */

// Repro for a transient vertex-buffer rebind bug. Every backend's vertex-stream
// rebind gate keys on (stream handle, startVertex) but not the layout handle:
// the shared hasVertexStreamChanged() in src/renderer.h (Vulkan, D3D11, Metal,
// WebGPU) and OpenGL's own inline copy in renderer_gl.cpp. All transient buffers
// share one handle, so two same-program draws that collide on startVertex with
// different-stride layouts look unchanged; the rebind is skipped and the second
// reuses the first's byte offset. Both triangles land on startVertex 6 (stride
// 16 at offset 96, stride 24 at offset 144); the blue one is missing on every
// backend until the gate also compares the layout handle.

#include "common.h"
#include "bgfx_utils.h"
#include "imgui/imgui.h"

namespace
{

struct PosColorVertex // stride 16
{
	float m_x;
	float m_y;
	float m_z;
	uint32_t m_abgr;

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position, 3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,   4, bgfx::AttribType::Uint8, true)
			.end();
	};

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout PosColorVertex::ms_layout;

struct PosColorTexVertex // stride 24; vs_cubes reads only position + color0
{
	float m_x;
	float m_y;
	float m_z;
	uint32_t m_abgr;
	float m_u;
	float m_v;

	static void init()
	{
		ms_layout
			.begin()
			.add(bgfx::Attrib::Position,  3, bgfx::AttribType::Float)
			.add(bgfx::Attrib::Color0,    4, bgfx::AttribType::Uint8, true)
			.add(bgfx::Attrib::TexCoord0, 2, bgfx::AttribType::Float)
			.end();
	};

	static bgfx::VertexLayout ms_layout;
};

bgfx::VertexLayout PosColorTexVertex::ms_layout;

class ExampleTransientLayoutBug : public entry::AppI
{
public:
	ExampleTransientLayoutBug(const char* _name, const char* _description, const char* _url)
		: entry::AppI(_name, _description, _url)
	{
	}

	void init(int32_t _argc, const char* const* _argv, uint32_t _width, uint32_t _height) override
	{
		Args args(_argc, _argv);

		m_width  = _width;
		m_height = _height;
		m_debug  = BGFX_DEBUG_TEXT;
		m_reset  = BGFX_RESET_VSYNC;

		bgfx::Init init;
		init.type     = args.m_type;
		init.vendorId = args.m_pciId;
		init.platformData.nwh  = entry::getNativeWindowHandle(entry::kDefaultWindowHandle);
		init.platformData.ndt  = entry::getNativeDisplayHandle();
		init.platformData.type = entry::getNativeWindowHandleType();
		init.resolution.width  = m_width;
		init.resolution.height = m_height;
		init.resolution.reset  = m_reset;
		bgfx::init(init);

		bgfx::setDebug(m_debug);

		bgfx::setViewClear(0
			, BGFX_CLEAR_COLOR|BGFX_CLEAR_DEPTH
			, 0x202020ff
			, 1.0f
			, 0
			);

		PosColorVertex::init();
		PosColorTexVertex::init();

		// Reuse the 01-cubes shaders (they read a_position + a_color0).
		m_program = loadProgram("vs_cubes", "fs_cubes");

		imguiCreate();
	}

	virtual int shutdown() override
	{
		imguiDestroy();
		bgfx::destroy(m_program);
		bgfx::shutdown();
		return 0;
	}

	bool update() override
	{
		if (!entry::processEvents(m_width, m_height, m_debug, m_reset, &m_mouseState) )
		{
			// Identity transform => positions are already in clip space.
			bgfx::setViewRect(0, 0, 0, uint16_t(m_width), uint16_t(m_height) );
			// Sequential => A is rendered immediately before B (no sort reorder).
			bgfx::setViewMode(0, bgfx::ViewMode::Sequential);
			bgfx::touch(0);

			// Must be the frame's first transient allocs (before ImGui) so the
			// write-offset starts at 0 and the startVertex collision is deterministic.
			const uint64_t state = BGFX_STATE_WRITE_RGB | BGFX_STATE_WRITE_A;

			// pad to offset 96 (6 * 16)
			if (6 == bgfx::getAvailTransientVertexBuffer(6, PosColorVertex::ms_layout) )
			{
				bgfx::TransientVertexBuffer pad;
				bgfx::allocTransientVertexBuffer(&pad, 6, PosColorVertex::ms_layout);
			}

			// A: stride 16, startVertex 6 (left, red)
			if (3 == bgfx::getAvailTransientVertexBuffer(3, PosColorVertex::ms_layout) )
			{
				bgfx::TransientVertexBuffer tvb;
				bgfx::allocTransientVertexBuffer(&tvb, 3, PosColorVertex::ms_layout);
				PosColorVertex* v = (PosColorVertex*)tvb.data;
				v[0] = { -0.8f, -0.5f, 0.0f, 0xff0000ff };
				v[1] = { -0.2f, -0.5f, 0.0f, 0xff0000ff };
				v[2] = { -0.5f,  0.5f, 0.0f, 0xff0000ff };
				bgfx::setVertexBuffer(0, &tvb);
				bgfx::setState(state);
				bgfx::submit(0, m_program);
			}

			// B: stride 24, startVertex 6 collides with A (right, blue)
			if (3 == bgfx::getAvailTransientVertexBuffer(3, PosColorTexVertex::ms_layout) )
			{
				bgfx::TransientVertexBuffer tvb;
				bgfx::allocTransientVertexBuffer(&tvb, 3, PosColorTexVertex::ms_layout);
				PosColorTexVertex* v = (PosColorTexVertex*)tvb.data;
				v[0] = { 0.2f, -0.5f, 0.0f, 0xffff0000, 0.0f, 0.0f };
				v[1] = { 0.8f, -0.5f, 0.0f, 0xffff0000, 0.0f, 0.0f };
				v[2] = { 0.5f,  0.5f, 0.0f, 0xffff0000, 0.0f, 0.0f };
				bgfx::setVertexBuffer(0, &tvb);
				bgfx::setState(state);
				bgfx::submit(0, m_program);
			}

			imguiBeginFrame(m_mouseState.m_mx
				,  m_mouseState.m_my
				, (m_mouseState.m_buttons[entry::MouseButton::Left  ] ? IMGUI_MBUT_LEFT   : 0)
				| (m_mouseState.m_buttons[entry::MouseButton::Right ] ? IMGUI_MBUT_RIGHT  : 0)
				| (m_mouseState.m_buttons[entry::MouseButton::Middle] ? IMGUI_MBUT_MIDDLE : 0)
				,  m_mouseState.m_mz
				, uint16_t(m_width)
				, uint16_t(m_height)
				);

			showExampleDialog(this);

			imguiEndFrame();

			bgfx::dbgTextClear();
			bgfx::dbgTextPrintf(0, 1, 0x0f, "Expect RED (left) + BLUE (right).");
			bgfx::dbgTextPrintf(0, 2, 0x0f, "Unfixed: the BLUE triangle is missing on every backend (VK/D3D11/Metal/WebGPU/GL).");

			bgfx::frame();

			return true;
		}

		return false;
	}

	entry::MouseState m_mouseState;

	uint32_t m_width;
	uint32_t m_height;
	uint32_t m_debug;
	uint32_t m_reset;
	bgfx::ProgramHandle m_program;
};

} // namespace

ENTRY_IMPLEMENT_MAIN(
	  ExampleTransientLayoutBug
	, "52-transientlayoutbug"
	, "Repro: transient vertex-layout rebind bug (issue #2562)."
	, "https://github.com/bkaradzic/bgfx/issues/2562"
	);
