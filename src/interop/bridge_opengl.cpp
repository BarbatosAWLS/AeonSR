#include "aeon_sr/interop/interop_bridges.hpp"

#include "aeon_sr/jitter/gl_jitter.hpp"

#include <windows.h>

#include <GL/gl.h>
#include <GL/glext.h>

#include <cstring>
#include <cwchar>
#include <vector>

namespace aeon_sr {
namespace {

template <typename T>
void safe_release(T *&p)
{
	if (p != nullptr) {
		p->Release();
		p = nullptr;
	}
}

typedef PROC(WINAPI *PfnWglGetProcAddress)(LPCSTR);
typedef HGLRC(WINAPI *PfnWglGetCurrentContext)(void);

template <class T>
bool load_gl(HMODULE gl32, PfnWglGetProcAddress wgl_get, T &fn, const char *name)
{
	PROC p = (wgl_get != nullptr) ? wgl_get(name) : nullptr;
	const uintptr_t v = reinterpret_cast<uintptr_t>(p);
	if (v <= 3 || v == static_cast<uintptr_t>(-1))
		p = (gl32 != nullptr) ? GetProcAddress(gl32, name) : nullptr;
	fn = reinterpret_cast<T>(p);
	return fn != nullptr;
}

std::wstring to_wide(const char *s)
{
	std::wstring w;
	for (const char *p = s; p != nullptr && *p != '\0'; ++p)
		w.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*p)));
	return w;
}

struct GlApi {
	GLenum(APIENTRY *GetError)(void) = nullptr;
	const GLubyte *(APIENTRY *GetString)(GLenum) = nullptr;
	void(APIENTRY *GetIntegerv)(GLenum, GLint *) = nullptr;
	void(APIENTRY *GenTextures)(GLsizei, GLuint *) = nullptr;
	void(APIENTRY *DeleteTextures)(GLsizei, const GLuint *) = nullptr;
	void(APIENTRY *BindTexture)(GLenum, GLuint) = nullptr;
	void(APIENTRY *TexParameteri)(GLenum, GLenum, GLint) = nullptr;
	void(APIENTRY *TexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum,
		const void *) = nullptr;
	void(APIENTRY *GetTexImage)(GLenum, GLint, GLenum, GLenum, void *) = nullptr;
	void(APIENTRY *PixelStorei)(GLenum, GLint) = nullptr;
	void(APIENTRY *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = nullptr;
	void(APIENTRY *ReadBuffer)(GLenum) = nullptr;
	void(APIENTRY *DrawBuffer)(GLenum) = nullptr;
	void(APIENTRY *Finish)(void) = nullptr;
	void(APIENTRY *Flush)(void) = nullptr;
	void(APIENTRY *Enable)(GLenum) = nullptr;
	void(APIENTRY *Disable)(GLenum) = nullptr;
	GLboolean(APIENTRY *IsEnabled)(GLenum) = nullptr;

	PFNGLGETSTRINGIPROC GetStringi = nullptr;
	PFNGLGENFRAMEBUFFERSPROC GenFramebuffers = nullptr;
	PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers = nullptr;
	PFNGLBINDFRAMEBUFFERPROC BindFramebuffer = nullptr;
	PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D = nullptr;
	PFNGLFRAMEBUFFERRENDERBUFFERPROC FramebufferRenderbuffer = nullptr;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC CheckFramebufferStatus = nullptr;
	PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer = nullptr;
	PFNGLGENBUFFERSPROC GenBuffers = nullptr;
	PFNGLDELETEBUFFERSPROC DeleteBuffers = nullptr;
	PFNGLBINDBUFFERPROC BindBuffer = nullptr;
	PFNGLBUFFERDATAPROC BufferData = nullptr;
	PFNGLTEXSTORAGE2DPROC TexStorage2D = nullptr;

	PFNGLGETUNSIGNEDBYTEVEXTPROC GetUnsignedBytevEXT = nullptr;
	PFNGLCREATEMEMORYOBJECTSEXTPROC CreateMemoryObjectsEXT = nullptr;
	PFNGLDELETEMEMORYOBJECTSEXTPROC DeleteMemoryObjectsEXT = nullptr;
	PFNGLMEMORYOBJECTPARAMETERIVEXTPROC MemoryObjectParameterivEXT = nullptr;
	PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC ImportMemoryWin32HandleEXT = nullptr;
	PFNGLTEXSTORAGEMEM2DEXTPROC TexStorageMem2DEXT = nullptr;

	PFNGLGENSEMAPHORESEXTPROC GenSemaphoresEXT = nullptr;
	PFNGLDELETESEMAPHORESEXTPROC DeleteSemaphoresEXT = nullptr;
	PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC ImportSemaphoreWin32HandleEXT = nullptr;
	PFNGLSEMAPHOREPARAMETERUI64VEXTPROC SemaphoreParameterui64vEXT = nullptr;
	PFNGLWAITSEMAPHOREEXTPROC WaitSemaphoreEXT = nullptr;
	PFNGLSIGNALSEMAPHOREEXTPROC SignalSemaphoreEXT = nullptr;

	const char *load_core(HMODULE gl32, PfnWglGetProcAddress wgl_get)
	{
		const char *missing = nullptr;
		const auto need = [&](auto &fn, const char *name) {
			if (!load_gl(gl32, wgl_get, fn, name) && missing == nullptr)
				missing = name;
		};
		need(GetError, "glGetError");
		need(GetString, "glGetString");
		need(GetIntegerv, "glGetIntegerv");
		need(GenTextures, "glGenTextures");
		need(DeleteTextures, "glDeleteTextures");
		need(BindTexture, "glBindTexture");
		need(TexParameteri, "glTexParameteri");
		need(TexSubImage2D, "glTexSubImage2D");
		need(GetTexImage, "glGetTexImage");
		need(PixelStorei, "glPixelStorei");
		need(ReadPixels, "glReadPixels");
		need(ReadBuffer, "glReadBuffer");
		need(DrawBuffer, "glDrawBuffer");
		need(Finish, "glFinish");
		need(Flush, "glFlush");
		need(Enable, "glEnable");
		need(Disable, "glDisable");
		need(IsEnabled, "glIsEnabled");

		need(GetStringi, "glGetStringi");
		need(GenFramebuffers, "glGenFramebuffers");
		need(DeleteFramebuffers, "glDeleteFramebuffers");
		need(BindFramebuffer, "glBindFramebuffer");
		need(FramebufferTexture2D, "glFramebufferTexture2D");
		need(FramebufferRenderbuffer, "glFramebufferRenderbuffer");
		need(CheckFramebufferStatus, "glCheckFramebufferStatus");
		need(BlitFramebuffer, "glBlitFramebuffer");
		need(GenBuffers, "glGenBuffers");
		need(DeleteBuffers, "glDeleteBuffers");
		need(BindBuffer, "glBindBuffer");
		need(BufferData, "glBufferData");
		need(TexStorage2D, "glTexStorage2D");
		return missing;
	}

	bool load_memory_object(HMODULE gl32, PfnWglGetProcAddress wgl_get)
	{
		bool ok = true;
		const auto need = [&](auto &fn, const char *name) {
			ok = load_gl(gl32, wgl_get, fn, name) && ok;
		};
		need(GetUnsignedBytevEXT, "glGetUnsignedBytevEXT");
		need(CreateMemoryObjectsEXT, "glCreateMemoryObjectsEXT");
		need(DeleteMemoryObjectsEXT, "glDeleteMemoryObjectsEXT");
		need(MemoryObjectParameterivEXT, "glMemoryObjectParameterivEXT");
		need(ImportMemoryWin32HandleEXT, "glImportMemoryWin32HandleEXT");
		need(TexStorageMem2DEXT, "glTexStorageMem2DEXT");
		return ok;
	}

	bool load_semaphore(HMODULE gl32, PfnWglGetProcAddress wgl_get)
	{
		bool ok = true;
		const auto need = [&](auto &fn, const char *name) {
			ok = load_gl(gl32, wgl_get, fn, name) && ok;
		};
		need(GenSemaphoresEXT, "glGenSemaphoresEXT");
		need(DeleteSemaphoresEXT, "glDeleteSemaphoresEXT");
		need(ImportSemaphoreWin32HandleEXT, "glImportSemaphoreWin32HandleEXT");
		need(SemaphoreParameterui64vEXT, "glSemaphoreParameterui64vEXT");
		need(WaitSemaphoreEXT, "glWaitSemaphoreEXT");
		need(SignalSemaphoreEXT, "glSignalSemaphoreEXT");
		return ok;
	}
};

constexpr const char *kSyncGpu = "shared fence";
constexpr const char *kSyncNoExtension = "CPU wait: no GL_EXT_semaphore_win32";
constexpr const char *kSyncImportRefused = "CPU wait: semaphore import refused";
constexpr const char *kSyncCheckFailed = "CPU wait: semaphore check failed";
constexpr const char *kSyncCallFailed = "CPU wait: semaphore call failed";

struct SharedSemaphore {
	ID3D12Fence *fence12 = nullptr;
	GLuint sem = 0;
	uint64_t value = 0;
};

GLenum drain_errors(const GlApi &gl)
{
	GLenum first = GL_NO_ERROR;
	for (int i = 0; i < 32 && gl.GetError != nullptr; ++i) {
		const GLenum e = gl.GetError();
		if (e == GL_NO_ERROR)
			break;
		if (first == GL_NO_ERROR)
			first = e;
	}
	return first;
}

const wchar_t *gl_error_name(GLenum e)
{
	switch (e) {
	case GL_NO_ERROR: return L"no error";
	case GL_INVALID_ENUM: return L"GL_INVALID_ENUM";
	case GL_INVALID_VALUE: return L"GL_INVALID_VALUE";
	case GL_INVALID_OPERATION: return L"GL_INVALID_OPERATION";
	case GL_OUT_OF_MEMORY: return L"GL_OUT_OF_MEMORY";
	case GL_INVALID_FRAMEBUFFER_OPERATION: return L"GL_INVALID_FRAMEBUFFER_OPERATION";
	default: return L"an unnamed GL error";
	}
}

bool has_extension(const GlApi &gl, const char *wanted)
{
	GLint n = 0;
	gl.GetIntegerv(GL_NUM_EXTENSIONS, &n);
	drain_errors(gl);
	for (GLint i = 0; i < n; ++i) {
		const GLubyte *const e = gl.GetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
		if (e != nullptr && strcmp(reinterpret_cast<const char *>(e), wanted) == 0) {
			drain_errors(gl);
			return true;
		}
	}
	drain_errors(gl);
	return false;
}

constexpr GLenum kPackStore[] = {
	GL_PACK_SWAP_BYTES, GL_PACK_LSB_FIRST, GL_PACK_ROW_LENGTH,
	GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PACK_ALIGNMENT,
};
constexpr GLenum kUnpackStore[] = {
	GL_UNPACK_SWAP_BYTES, GL_UNPACK_LSB_FIRST, GL_UNPACK_ROW_LENGTH,
	GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_UNPACK_ALIGNMENT,
};
constexpr GLint kStoreDefault[] = { 0, 0, 0, 0, 0, 4 };
constexpr int kStoreCount = static_cast<int>(sizeof(kStoreDefault) / sizeof(kStoreDefault[0]));

class StateGuard {
public:
	explicit StateGuard(const GlApi &gl) : gl_(gl)
	{
		drain_errors(gl_);
		gl_.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo_);
		gl_.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo_);
		gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		gl_.GetIntegerv(GL_READ_BUFFER, &read_buffer_);
		gl_.GetIntegerv(GL_DRAW_BUFFER, &draw_buffer_);
		gl_.GetIntegerv(GL_TEXTURE_BINDING_2D, &texture_2d_);
		gl_.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer_);
		gl_.GetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer_);
		for (int i = 0; i < kStoreCount; ++i) {
			gl_.GetIntegerv(kPackStore[i], &pack_[i]);
			gl_.GetIntegerv(kUnpackStore[i], &unpack_[i]);
		}
		scissor_ = gl_.IsEnabled(GL_SCISSOR_TEST);
		srgb_ = gl_.IsEnabled(GL_FRAMEBUFFER_SRGB);

		gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		for (int i = 0; i < kStoreCount; ++i) {
			gl_.PixelStorei(kPackStore[i], kStoreDefault[i]);
			gl_.PixelStorei(kUnpackStore[i], kStoreDefault[i]);
		}
		if (scissor_ != GL_FALSE)
			gl_.Disable(GL_SCISSOR_TEST);
		if (srgb_ != GL_FALSE)
			gl_.Disable(GL_FRAMEBUFFER_SRGB);
		drain_errors(gl_);
	}

	~StateGuard()
	{
		gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
		gl_.ReadBuffer(static_cast<GLenum>(read_buffer_));
		gl_.DrawBuffer(static_cast<GLenum>(draw_buffer_));
		for (int i = 0; i < kStoreCount; ++i) {
			gl_.PixelStorei(kPackStore[i], pack_[i]);
			gl_.PixelStorei(kUnpackStore[i], unpack_[i]);
		}
		gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer_));
		gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer_));
		gl_.BindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture_2d_));
		if (scissor_ != GL_FALSE)
			gl_.Enable(GL_SCISSOR_TEST);
		if (srgb_ != GL_FALSE)
			gl_.Enable(GL_FRAMEBUFFER_SRGB);
		gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo_));
		gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo_));
		drain_errors(gl_);
	}

	StateGuard(const StateGuard &) = delete;
	StateGuard &operator=(const StateGuard &) = delete;

private:
	const GlApi &gl_;
	GLint read_fbo_ = 0;
	GLint draw_fbo_ = 0;
	GLint read_buffer_ = GL_BACK;
	GLint draw_buffer_ = GL_BACK;
	GLint texture_2d_ = 0;
	GLint pack_buffer_ = 0;
	GLint unpack_buffer_ = 0;
	GLint pack_[kStoreCount]{};
	GLint unpack_[kStoreCount]{};
	GLboolean scissor_ = GL_FALSE;
	GLboolean srgb_ = GL_FALSE;
};

struct GlFormat {
	GLenum internal_format;
	GLenum transfer_format;
	GLenum transfer_type;
};

bool gl_format_of(DXGI_FORMAT fmt, GlFormat &out) noexcept
{
	switch (fmt) {
	case DXGI_FORMAT_R8G8B8A8_UNORM: out = { GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE }; return true;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: out = { GL_SRGB8_ALPHA8, GL_RGBA, GL_UNSIGNED_BYTE }; return true;
	case DXGI_FORMAT_R10G10B10A2_UNORM:
		out = { GL_RGB10_A2, GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV };
		return true;
	case DXGI_FORMAT_R11G11B10_FLOAT:
		out = { GL_R11F_G11F_B10F, GL_RGB, GL_UNSIGNED_INT_10F_11F_11F_REV };
		return true;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: out = { GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT }; return true;
	case DXGI_FORMAT_R16G16B16A16_UNORM: out = { GL_RGBA16, GL_RGBA, GL_UNSIGNED_SHORT }; return true;
	case DXGI_FORMAT_R32G32B32A32_FLOAT: out = { GL_RGBA32F, GL_RGBA, GL_FLOAT }; return true;
	case DXGI_FORMAT_R16G16_FLOAT: out = { GL_RG16F, GL_RG, GL_HALF_FLOAT }; return true;
	case DXGI_FORMAT_R16G16_UNORM: out = { GL_RG16, GL_RG, GL_UNSIGNED_SHORT }; return true;
	case DXGI_FORMAT_R32G32_FLOAT: out = { GL_RG32F, GL_RG, GL_FLOAT }; return true;
	case DXGI_FORMAT_R8G8_UNORM: out = { GL_RG8, GL_RG, GL_UNSIGNED_BYTE }; return true;
	case DXGI_FORMAT_R16_FLOAT: out = { GL_R16F, GL_RED, GL_HALF_FLOAT }; return true;
	case DXGI_FORMAT_R32_FLOAT: out = { GL_R32F, GL_RED, GL_FLOAT }; return true;
	case DXGI_FORMAT_R8_UNORM: out = { GL_R8, GL_RED, GL_UNSIGNED_BYTE }; return true;
	default: return false;
	}
}

DXGI_FORMAT plane_format_of(reshade::api::format fmt) noexcept
{
	switch (fmt) {
	case reshade::api::format::r8g8b8a8_unorm:
	case reshade::api::format::r8g8b8a8_unorm_srgb:
	case reshade::api::format::r8g8b8a8_typeless:
	case reshade::api::format::r8g8b8x8_unorm:
	case reshade::api::format::r8g8b8x8_unorm_srgb:
	case reshade::api::format::b8g8r8a8_unorm:
	case reshade::api::format::b8g8r8a8_unorm_srgb:
	case reshade::api::format::b8g8r8a8_typeless:
	case reshade::api::format::b8g8r8x8_unorm:
	case reshade::api::format::b8g8r8x8_unorm_srgb:
	case reshade::api::format::b8g8r8x8_typeless:
	case reshade::api::format::b5g6r5_unorm:
	case reshade::api::format::b5g5r5a1_unorm:
		return DXGI_FORMAT_R8G8B8A8_UNORM;
	case reshade::api::format::r10g10b10a2_unorm:
	case reshade::api::format::r10g10b10a2_typeless:
	case reshade::api::format::b10g10r10a2_unorm:
		return DXGI_FORMAT_R10G10B10A2_UNORM;
	case reshade::api::format::r11g11b10_float:
		return DXGI_FORMAT_R11G11B10_FLOAT;
	case reshade::api::format::r16g16b16a16_unorm:
		return DXGI_FORMAT_R16G16B16A16_UNORM;
	case reshade::api::format::r32g32b32a32_float:
		return DXGI_FORMAT_R32G32B32A32_FLOAT;
	case reshade::api::format::r16g16_float:
		return DXGI_FORMAT_R16G16_FLOAT;
	case reshade::api::format::r32g32_float:
		return DXGI_FORMAT_R32G32_FLOAT;
	case reshade::api::format::r8g8_unorm:
		return DXGI_FORMAT_R8G8_UNORM;
	case reshade::api::format::r8_unorm:
		return DXGI_FORMAT_R8_UNORM;
	case reshade::api::format::r16_float:
		return DXGI_FORMAT_R16_FLOAT;
	case reshade::api::format::r32_float:
		return DXGI_FORMAT_R32_FLOAT;
	default:
		return DXGI_FORMAT_R16G16B16A16_FLOAT;
	}
}

struct PlaneSlot {
	SharedPlane shared{};
	GLuint tex = 0;
	GLuint mem = 0;
	GLenum internal_format = 0;
};

struct ScratchTex {
	GLuint tex = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	GLenum internal_format = 0;
};

struct ReadableSlot {
	const SharedPlane *owner = nullptr;
	GLuint tex = 0;
	GLuint mem = 0;
};

class BridgeOpenGl final : public FrameBridge {
public:
	~BridgeOpenGl() override { shutdown(); }

	BridgeKind kind() const noexcept override { return BridgeKind::OpenGl; }
	const char *name() const noexcept override { return "OpenGL through GL_EXT_memory_object_win32"; }

	const char *sync_name() const noexcept override { return sync_; }

	bool init(reshade::api::device *game, EngineDevice &engine) override
	{
		const GlJitterQuiet quiet;
		if (game == nullptr || game->get_api() != reshade::api::device_api::opengl) {
			last_error = L"not an OpenGL device";
			return false;
		}
		if (!engine.ready()) {
			last_error = engine.last_error.empty()
				? std::wstring(L"the engine device is not ready") : engine.last_error;
			return false;
		}

		gl32_ = GetModuleHandleW(L"opengl32.dll");
		if (gl32_ == nullptr) {
			last_error = L"opengl32.dll is not loaded in this process, so the OpenGL entry "
				L"points the bridge needs cannot be found.";
			return false;
		}
		load_gl(gl32_, nullptr, wgl_get_proc_, "wglGetProcAddress");
		load_gl(gl32_, nullptr, wgl_current_, "wglGetCurrentContext");
		if (wgl_get_proc_ == nullptr || wgl_current_ == nullptr) {
			last_error = L"opengl32.dll does not export wglGetProcAddress, so no OpenGL "
				L"function can be resolved. The file is very likely not the system one.";
			shutdown();
			return false;
		}

		gl_context_ = wgl_current_();
		if (gl_context_ == nullptr) {
			last_error = L"no OpenGL context was current when the add-on was set up, so the "
				L"bridge had nothing to resolve its entry points against.";
			shutdown();
			return false;
		}

		if (const char *const missing = gl_.load_core(gl32_, wgl_get_proc_)) {
			last_error = L"this OpenGL driver does not provide " + to_wide(missing) +
				L", which the add-on needs. An OpenGL 4.3 driver has it, so this one is "
				L"either very old or a software renderer.";
			shutdown();
			return false;
		}

		GLint major = 0;
		GLint minor = 0;
		gl_.GetIntegerv(GL_MAJOR_VERSION, &major);
		gl_.GetIntegerv(GL_MINOR_VERSION, &minor);
		drain_errors(gl_);
		if (major < 4 || (major == 4 && minor < 3)) {
			wchar_t buf[192];
			swprintf(buf, sizeof(buf) / sizeof(buf[0]),
				L"this OpenGL context is version %d.%d and the add-on needs 4.3 or newer.",
				static_cast<int>(major), static_cast<int>(minor));
			last_error = buf;
			shutdown();
			return false;
		}

		if (!has_extension(gl_, "GL_EXT_memory_object") ||
			!has_extension(gl_, "GL_EXT_memory_object_win32") ||
			!gl_.load_memory_object(gl32_, wgl_get_proc_)) {
			last_error = L"this OpenGL driver does not support GL_EXT_memory_object_win32, "
				L"which is how the add-on shares the frame with Direct3D 12. Update the "
				L"graphics driver; until then the frame has to travel through system memory.";
			shutdown();
			return false;
		}

		GLubyte luid[GL_LUID_SIZE_EXT]{};
		gl_.GetUnsignedBytevEXT(GL_DEVICE_LUID_EXT, luid);
		const bool luid_read = drain_errors(gl_) == GL_NO_ERROR;
		if (!luid_read || !engine.adapter_matched() ||
			memcmp(&engine.luid(), luid, sizeof(LUID)) != 0) {
			last_error = L"the OpenGL context and the add-on's Direct3D 12 engine ended up on "
				L"different GPUs, and a shared texture cannot cross two of them. Force the "
				L"game onto the same GPU in the graphics driver's control panel.";
			shutdown();
			return false;
		}

		engine_ = &engine;
		device_ = game;

		StateGuard guard(gl_);
		gl_.GenFramebuffers(1, &fbo_read_);
		gl_.GenFramebuffers(1, &fbo_draw_);
		gl_.GenFramebuffers(1, &fbo_depth_);
		if (drain_errors(gl_) != GL_NO_ERROR || fbo_read_ == 0 || fbo_draw_ == 0 || fbo_depth_ == 0) {
			last_error = L"this OpenGL driver refused to create a framebuffer object, which is "
				L"how the frame is carried in and out.";
			shutdown();
			return false;
		}
		gl_.BindFramebuffer(GL_FRAMEBUFFER, fbo_depth_);
		gl_.ReadBuffer(GL_NONE);
		gl_.DrawBuffer(GL_NONE);
		drain_errors(gl_);

		if (!verify_route()) {
			shutdown();
			return false;
		}
		setup_gpu_sync();
		return true;
	}

	void shutdown() override
	{
		const GlJitterQuiet quiet;
		if (engine_ != nullptr)
			engine_->wait_cpu(engine_->last_submitted());

		if (gl_alive()) {
			for (ReadableSlot &r : readables_)
				delete_gl_plane(r.tex, r.mem);
			delete_gl_plane(color_.tex, color_.mem);
			delete_gl_plane(depth_.tex, depth_.mem);
			delete_gl_plane(motion_.tex, motion_.mem);
			delete_scratch(resolve_);
			delete_scratch(depth_scratch_);
			if (pbo_ != 0)
				gl_.DeleteBuffers(1, &pbo_);
			if (fbo_read_ != 0)
				gl_.DeleteFramebuffers(1, &fbo_read_);
			if (fbo_draw_ != 0)
				gl_.DeleteFramebuffers(1, &fbo_draw_);
			if (fbo_depth_ != 0)
				gl_.DeleteFramebuffers(1, &fbo_depth_);
			delete_semaphores();
			drain_errors(gl_);
		}
		to_engine_.sem = to_game_.sem = 0;
		safe_release(to_engine_.fence12);
		safe_release(to_game_.fence12);
		to_engine_.value = to_game_.value = 0;
		gpu_sync_ = false;
		sync_ = kSyncNoExtension;
		readables_.clear();
		color_.tex = color_.mem = 0;
		depth_.tex = depth_.mem = 0;
		motion_.tex = motion_.mem = 0;
		resolve_ = ScratchTex{};
		depth_scratch_ = ScratchTex{};
		pbo_ = 0;
		pbo_bytes_ = 0;
		fbo_read_ = fbo_draw_ = fbo_depth_ = 0;

		bridge_util::release_plane(color_.shared);
		bridge_util::release_plane(depth_.shared);
		bridge_util::release_plane(motion_.shared);

		gl_ = GlApi{};
		wgl_get_proc_ = nullptr;
		wgl_current_ = nullptr;
		gl_context_ = nullptr;
		gl32_ = nullptr;
		engine_ = nullptr;
		device_ = nullptr;
	}

	bool ready() const noexcept override
	{
		return engine_ != nullptr && device_ != nullptr && gl_context_ != nullptr &&
			gl_.TexStorageMem2DEXT != nullptr && fbo_read_ != 0;
	}

	BridgeStep begin(reshade::api::effect_runtime *, reshade::api::command_list *,
		const BridgeInputs &in, BridgeFrame &out) override
	{
		const GlJitterQuiet quiet;
		out = BridgeFrame{};
		if (!ready())
			return BridgeStep::Init;
		if (!adopt_current_context())
			return BridgeStep::Init;
		if (in.color.handle == 0) {
			last_error = L"ReShade did not name a colour buffer this frame.";
			return BridgeStep::Import;
		}

		StateGuard guard(gl_);

		const reshade::api::resource_desc cdesc = device_->get_resource_desc(in.color);
		const uint32_t w = cdesc.texture.width;
		const uint32_t h = cdesc.texture.height;
		if (w == 0 || h == 0) {
			last_error = L"the game's colour buffer has no size this frame.";
			return BridgeStep::Import;
		}
		if (!ensure_plane(color_, w, h, plane_format_of(cdesc.texture.format), true))
			return BridgeStep::Import;
		if (copy_in(in.color, color_, w, h, cdesc.texture.samples) != BridgeStep::Ok)
			return BridgeStep::Import;

		bool have_depth = false;
		if (in.depth.handle != 0) {
			const reshade::api::resource_desc ddesc = device_->get_resource_desc(in.depth);
			if (ddesc.texture.samples > 1) {
				last_error = L"the game's depth buffer is multisampled and this bridge cannot "
					L"carry it; the upscaler runs without depth.";
			} else if (ddesc.texture.width != 0 && ddesc.texture.height != 0 &&
				ensure_plane(depth_, ddesc.texture.width, ddesc.texture.height, DXGI_FORMAT_R32_FLOAT)) {
				have_depth = copy_in_depth(in.depth, depth_, ddesc.texture.width,
					ddesc.texture.height) == BridgeStep::Ok;
			}
		}

		bool have_motion = false;
		if (in.motion_vectors.handle != 0) {
			const reshade::api::resource_desc mdesc = device_->get_resource_desc(in.motion_vectors);
			if (mdesc.texture.width != 0 && mdesc.texture.height != 0 &&
				ensure_plane(motion_, mdesc.texture.width, mdesc.texture.height,
					plane_format_of(mdesc.texture.format)))
				have_motion = copy_in(in.motion_vectors, motion_, mdesc.texture.width,
					mdesc.texture.height, mdesc.texture.samples) == BridgeStep::Ok;
		}

		const bool handed = gpu_sync_ && hand_to_engine(have_depth, have_motion);
		if (!handed)
			gl_.Finish();

		out.cmd = engine_->begin_list();
		if (out.cmd == nullptr) {
			last_error = engine_->last_error;
			if (gpu_sync_ && to_engine_.fence12 != nullptr) {
				to_engine_.fence12->Signal(to_engine_.value);
				lose_gpu_sync();
			}
			return BridgeStep::Begin;
		}
		out.color = color_.shared.engine;
		out.depth = have_depth ? depth_.shared.engine : nullptr;
		out.motion_vectors = have_motion ? motion_.shared.engine : nullptr;
		out.width = color_.shared.width;
		out.height = color_.shared.height;
		return BridgeStep::Ok;
	}

	BridgeStep end(reshade::api::effect_runtime *, reshade::api::command_list *,
		reshade::api::resource game_color) override
	{
		const GlJitterQuiet quiet;
		if (!ready())
			return BridgeStep::Init;
		if (!engine_->submit_list()) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}
		const bool handed = gpu_sync_ && same_context() && hand_to_game();
		if (!handed && !engine_->wait_cpu(engine_->last_submitted())) {
			last_error = engine_->last_error;
			return BridgeStep::Finish;
		}

		if (game_color.handle == 0 || color_.tex == 0)
			return BridgeStep::Export;
		if (!same_context()) {
			last_error = L"the game changed its OpenGL context mid frame, so the finished "
				L"picture had nowhere to go.";
			return BridgeStep::Export;
		}

		StateGuard guard(gl_);
		const reshade::api::resource_desc desc = device_->get_resource_desc(game_color);
		if (desc.texture.width != color_.shared.width || desc.texture.height != color_.shared.height) {
			last_error = L"the game's colour buffer changed size while the frame was being "
				L"upscaled; this one is dropped and the next is built at the new size.";
			return BridgeStep::Export;
		}
		return copy_out(color_, game_color, color_.shared.width, color_.shared.height);
	}

	bool request_readable(SharedPlane &plane, uint32_t w, uint32_t h, DXGI_FORMAT fmt) override
	{
		const GlJitterQuiet quiet;
		if (!ready() || w == 0 || h == 0)
			return false;

		ReadableSlot *slot = nullptr;
		for (ReadableSlot &r : readables_) {
			if (r.owner == &plane) {
				slot = &r;
				break;
			}
		}
		if (slot != nullptr && plane.matches(w, h, fmt))
			return true;

		if (!same_context()) {
			last_error = L"the plane the add-on wanted to share with OpenGL was asked for "
				L"while another context was current.";
			return false;
		}

		StateGuard guard(gl_);
		engine_->wait_cpu(engine_->last_submitted());

		if (slot != nullptr) {
			delete_gl_plane(slot->tex, slot->mem);
		} else {
			readables_.push_back(ReadableSlot{ &plane, 0, 0 });
			slot = &readables_.back();
		}
		bridge_util::release_plane(plane);

		PlaneSlot built{};
		if (make_plane(built, w, h, fmt, false)) {
			slot->tex = built.tex;
			slot->mem = built.mem;
			plane = built.shared;
			return true;
		}

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = fmt;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		if (FAILED(engine_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
				SharedPlane::kState, nullptr, IID_PPV_ARGS(&plane.engine)))) {
			last_error = L"the engine device refused a texture.";
			return false;
		}
		plane.width = w;
		plane.height = h;
		plane.format = fmt;
		return true;
	}

private:
	bool same_context() const noexcept
	{
		return wgl_current_ != nullptr && gl_context_ != nullptr && wgl_current_() == gl_context_;
	}

	bool gl_alive() const noexcept { return same_context() && gl_.DeleteTextures != nullptr; }

	bool adopt_current_context()
	{
		if (wgl_current_ == nullptr)
			return false;
		const HGLRC now = wgl_current_();
		if (now == gl_context_)
			return true;
		if (now == nullptr) {
			last_error = L"no OpenGL context is current on the thread the add-on runs on.";
			return false;
		}

		engine_->wait_cpu(engine_->last_submitted());
		readables_.clear();
		color_.tex = color_.mem = 0;
		depth_.tex = depth_.mem = 0;
		motion_.tex = motion_.mem = 0;
		resolve_ = ScratchTex{};
		depth_scratch_ = ScratchTex{};
		pbo_ = 0;
		pbo_bytes_ = 0;
		fbo_read_ = fbo_draw_ = fbo_depth_ = 0;
		bridge_util::release_plane(color_.shared);
		bridge_util::release_plane(depth_.shared);
		bridge_util::release_plane(motion_.shared);

		to_engine_.sem = to_game_.sem = 0;
		gl_context_ = now;
		if (gl_.load_core(gl32_, wgl_get_proc_) != nullptr || !gl_.load_memory_object(gl32_, wgl_get_proc_)) {
			last_error = L"the game switched to an OpenGL context that does not have the "
				L"extensions the add-on needs.";
			gl_context_ = nullptr;
			return false;
		}

		StateGuard guard(gl_);
		gl_.GenFramebuffers(1, &fbo_read_);
		gl_.GenFramebuffers(1, &fbo_draw_);
		gl_.GenFramebuffers(1, &fbo_depth_);
		gl_.BindFramebuffer(GL_FRAMEBUFFER, fbo_depth_);
		gl_.ReadBuffer(GL_NONE);
		gl_.DrawBuffer(GL_NONE);
		if (drain_errors(gl_) != GL_NO_ERROR || fbo_read_ == 0 || fbo_draw_ == 0 || fbo_depth_ == 0) {
			last_error = L"the game's new OpenGL context refused a framebuffer object.";
			if (gpu_sync_) {
				gpu_sync_ = false;
				sync_ = kSyncImportRefused;
			}
			return false;
		}
		if (gpu_sync_ && to_engine_.fence12 != nullptr && to_engine_.fence12->GetCompletedValue() < to_engine_.value) {
			HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (ev != nullptr && SUCCEEDED(to_engine_.fence12->SetEventOnCompletion(to_engine_.value, ev)))
				WaitForSingleObject(ev, 1000);
			if (ev != nullptr)
				CloseHandle(ev);
		}
		if (gpu_sync_ && (!gl_.load_semaphore(gl32_, wgl_get_proc_) || !import_semaphores() || !check_semaphores())) {
			delete_semaphores();
			gpu_sync_ = false;
			sync_ = kSyncImportRefused;
		}
		return true;
	}

	void setup_gpu_sync()
	{
		gpu_sync_ = false;
		if (!has_extension(gl_, "GL_EXT_semaphore") || !has_extension(gl_, "GL_EXT_semaphore_win32") ||
			!gl_.load_semaphore(gl32_, wgl_get_proc_)) {
			sync_ = kSyncNoExtension;
			return;
		}
		const auto give_up = [this](const char *why) {
			delete_semaphores();
			safe_release(to_engine_.fence12);
			safe_release(to_game_.fence12);
			sync_ = why;
		};
		if (FAILED(engine_->device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&to_engine_.fence12))) ||
			FAILED(engine_->device()->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&to_game_.fence12))) ||
			!import_semaphores()) {
			give_up(kSyncImportRefused);
			return;
		}
		if (!check_semaphores()) {
			give_up(kSyncCheckFailed);
			return;
		}
		gpu_sync_ = true;
		sync_ = kSyncGpu;
	}

	bool import_semaphores()
	{
		for (SharedSemaphore *s : { &to_engine_, &to_game_ }) {
			HANDLE handle = nullptr;
			if (s->fence12 == nullptr ||
				FAILED(engine_->device()->CreateSharedHandle(s->fence12, nullptr, GENERIC_ALL, nullptr, &handle)) ||
				handle == nullptr)
				return false;
			drain_errors(gl_);
			gl_.GenSemaphoresEXT(1, &s->sem);
			gl_.ImportSemaphoreWin32HandleEXT(s->sem, GL_HANDLE_TYPE_D3D12_FENCE_EXT, handle);
			CloseHandle(handle);
			if (drain_errors(gl_) != GL_NO_ERROR || s->sem == 0)
				return false;
		}
		return true;
	}

	bool check_semaphores()
	{
		const uint64_t v = to_engine_.value + 1;
		drain_errors(gl_);
		gl_.SemaphoreParameterui64vEXT(to_engine_.sem, GL_D3D12_FENCE_VALUE_EXT, &v);
		gl_.SignalSemaphoreEXT(to_engine_.sem, 0, nullptr, 0, nullptr, nullptr);
		gl_.Flush();
		to_engine_.value = v;
		if (drain_errors(gl_) != GL_NO_ERROR)
			return false;
		bool seen = to_engine_.fence12->GetCompletedValue() >= v;
		if (!seen) {
			HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (ev != nullptr && SUCCEEDED(to_engine_.fence12->SetEventOnCompletion(v, ev)))
				seen = WaitForSingleObject(ev, 1000) == WAIT_OBJECT_0;
			if (ev != nullptr)
				CloseHandle(ev);
		}
		return seen;
	}

	void delete_semaphores()
	{
		if (gl_.DeleteSemaphoresEXT != nullptr && same_context()) {
			if (to_engine_.sem != 0)
				gl_.DeleteSemaphoresEXT(1, &to_engine_.sem);
			if (to_game_.sem != 0)
				gl_.DeleteSemaphoresEXT(1, &to_game_.sem);
		}
		to_engine_.sem = to_game_.sem = 0;
	}

	void lose_gpu_sync()
	{
		gpu_sync_ = false;
		sync_ = kSyncCallFailed;
	}

	bool hand_to_engine(bool have_depth, bool have_motion)
	{
		if (to_engine_.sem == 0)
			return false;
		GLuint textures[3] = { color_.tex, 0, 0 };
		GLuint count = 1;
		if (have_depth && depth_.tex != 0)
			textures[count++] = depth_.tex;
		if (have_motion && motion_.tex != 0)
			textures[count++] = motion_.tex;
		const GLenum layouts[3] = { GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT, GL_LAYOUT_GENERAL_EXT };
		const uint64_t v = to_engine_.value + 1;
		drain_errors(gl_);
		gl_.SemaphoreParameterui64vEXT(to_engine_.sem, GL_D3D12_FENCE_VALUE_EXT, &v);
		gl_.SignalSemaphoreEXT(to_engine_.sem, 0, nullptr, count, textures, layouts);
		gl_.Flush();
		if (drain_errors(gl_) != GL_NO_ERROR) {
			lose_gpu_sync();
			return false;
		}
		to_engine_.value = v;
		return SUCCEEDED(engine_->queue()->Wait(to_engine_.fence12, v));
	}

	bool hand_to_game()
	{
		if (to_game_.sem == 0)
			return false;
		const uint64_t v = to_game_.value + 1;
		if (FAILED(engine_->queue()->Signal(to_game_.fence12, v)))
			return false;
		to_game_.value = v;
		std::vector<GLuint> textures;
		textures.reserve(readables_.size() + 1);
		if (color_.tex != 0)
			textures.push_back(color_.tex);
		for (const ReadableSlot &r : readables_)
			if (r.tex != 0)
				textures.push_back(r.tex);
		const std::vector<GLenum> layouts(textures.size(), GL_LAYOUT_GENERAL_EXT);
		drain_errors(gl_);
		gl_.SemaphoreParameterui64vEXT(to_game_.sem, GL_D3D12_FENCE_VALUE_EXT, &v);
		gl_.WaitSemaphoreEXT(to_game_.sem, 0, nullptr, static_cast<GLuint>(textures.size()), textures.data(),
			layouts.data());
		if (drain_errors(gl_) != GL_NO_ERROR) {
			lose_gpu_sync();
			return false;
		}
		return true;
	}

	void delete_gl_plane(GLuint &tex, GLuint &mem)
	{
		if (tex != 0)
			gl_.DeleteTextures(1, &tex);
		if (mem != 0)
			gl_.DeleteMemoryObjectsEXT(1, &mem);
		tex = 0;
		mem = 0;
	}

	void delete_scratch(ScratchTex &s)
	{
		if (s.tex != 0)
			gl_.DeleteTextures(1, &s.tex);
		s = ScratchTex{};
	}

	bool make_plane(PlaneSlot &slot, uint32_t w, uint32_t h, DXGI_FORMAT fmt, bool render_target)
	{
		GlFormat glf{};
		if (!gl_format_of(fmt, glf)) {
			wchar_t buf[192];
			swprintf(buf, sizeof(buf) / sizeof(buf[0]),
				L"OpenGL has no name for DXGI format %u, so that plane cannot be shared with "
				L"the game.", static_cast<unsigned>(fmt));
			last_error = buf;
			return false;
		}

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = w;
		desc.Height = h;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = fmt;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET
			: D3D12_RESOURCE_FLAG_NONE;

		ID3D12Resource *res = nullptr;
		if (FAILED(engine_->device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
				SharedPlane::kState, nullptr, IID_PPV_ARGS(&res))) || res == nullptr) {
			last_error = L"the engine device refused a shareable texture. The GPU may be out "
				L"of memory.";
			return false;
		}
		HANDLE handle = nullptr;
		if (FAILED(engine_->device()->CreateSharedHandle(res, nullptr, GENERIC_ALL, nullptr, &handle)) ||
			handle == nullptr) {
			safe_release(res);
			last_error = L"the engine device refused to share a texture, so it cannot be "
				L"imported into OpenGL.";
			return false;
		}

		const D3D12_RESOURCE_ALLOCATION_INFO info = engine_->device()->GetResourceAllocationInfo(0, 1, &desc);

		drain_errors(gl_);
		GLuint mem = 0;
		GLuint tex = 0;
		gl_.CreateMemoryObjectsEXT(1, &mem);
		const GLint dedicated = GL_TRUE;
		gl_.MemoryObjectParameterivEXT(mem, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
		gl_.ImportMemoryWin32HandleEXT(mem, static_cast<GLuint64>(info.SizeInBytes),
			GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, handle);
		GLenum err = drain_errors(gl_);
		if (err == GL_NO_ERROR) {
			gl_.GenTextures(1, &tex);
			gl_.BindTexture(GL_TEXTURE_2D, tex);
			gl_.TexStorageMem2DEXT(GL_TEXTURE_2D, 1, glf.internal_format,
				static_cast<GLsizei>(w), static_cast<GLsizei>(h), mem, 0);
			err = drain_errors(gl_);
		}
		CloseHandle(handle);

		if (err != GL_NO_ERROR || tex == 0) {
			delete_gl_plane(tex, mem);
			drain_errors(gl_);
			safe_release(res);
			last_error = std::wstring(L"this OpenGL driver refused to import the shared "
				L"texture (") + gl_error_name(err) + L").";
			return false;
		}

		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		drain_errors(gl_);

		slot.tex = tex;
		slot.mem = mem;
		slot.internal_format = glf.internal_format;
		slot.shared.engine = res;
		slot.shared.width = w;
		slot.shared.height = h;
		slot.shared.format = fmt;
		slot.shared.game = { (static_cast<uint64_t>(GL_TEXTURE_2D) << 40) | tex };
		return true;
	}

	bool ensure_plane(PlaneSlot &slot, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
		bool render_target = false)
	{
		if (slot.shared.matches(w, h, fmt))
			return true;
		engine_->wait_cpu(engine_->last_submitted());
		delete_gl_plane(slot.tex, slot.mem);
		bridge_util::release_plane(slot.shared);
		slot.internal_format = 0;
		return make_plane(slot, w, h, fmt, render_target);
	}

	bool ensure_scratch(ScratchTex &s, uint32_t w, uint32_t h, GLenum internal_format)
	{
		if (s.tex != 0 && s.width == w && s.height == h && s.internal_format == internal_format)
			return true;
		delete_scratch(s);
		drain_errors(gl_);
		gl_.GenTextures(1, &s.tex);
		gl_.BindTexture(GL_TEXTURE_2D, s.tex);
		gl_.TexStorage2D(GL_TEXTURE_2D, 1, internal_format,
			static_cast<GLsizei>(w), static_cast<GLsizei>(h));
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		if (drain_errors(gl_) != GL_NO_ERROR || s.tex == 0) {
			delete_scratch(s);
			drain_errors(gl_);
			last_error = L"this OpenGL driver refused a working texture the bridge needs.";
			return false;
		}
		s.width = w;
		s.height = h;
		s.internal_format = internal_format;
		return true;
	}

	bool ensure_pbo(size_t bytes)
	{
		if (pbo_ != 0 && pbo_bytes_ >= bytes)
			return true;
		drain_errors(gl_);
		if (pbo_ == 0)
			gl_.GenBuffers(1, &pbo_);
		gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
		gl_.BufferData(GL_PIXEL_PACK_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, GL_STREAM_COPY);
		gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		if (drain_errors(gl_) != GL_NO_ERROR || pbo_ == 0) {
			if (pbo_ != 0)
				gl_.DeleteBuffers(1, &pbo_);
			pbo_ = 0;
			pbo_bytes_ = 0;
			drain_errors(gl_);
			last_error = L"this OpenGL driver refused the buffer the depth copy needs.";
			return false;
		}
		pbo_bytes_ = bytes;
		return true;
	}

	bool framebuffer_ok(GLenum target)
	{
		if (gl_.CheckFramebufferStatus(target) == GL_FRAMEBUFFER_COMPLETE)
			return drain_errors(gl_) == GL_NO_ERROR;
		drain_errors(gl_);
		last_error = L"one of the game's buffers could not be attached to a framebuffer, so "
			L"the frame could not be carried across.";
		return false;
	}

	bool bind_read_source(reshade::api::resource src)
	{
		const GLenum target = static_cast<GLenum>(src.handle >> 40);
		const GLuint object = static_cast<GLuint>(src.handle & 0xFFFFFFFFu);
		if (target == GL_FRAMEBUFFER_DEFAULT) {
			gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
			gl_.ReadBuffer(object);
			return drain_errors(gl_) == GL_NO_ERROR;
		}
		gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_read_);
		if (target == GL_RENDERBUFFER)
			gl_.FramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, object);
		else
			gl_.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, object, 0);
		gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
		return framebuffer_ok(GL_READ_FRAMEBUFFER);
	}

	bool bind_draw_dest(reshade::api::resource dst)
	{
		const GLenum target = static_cast<GLenum>(dst.handle >> 40);
		const GLuint object = static_cast<GLuint>(dst.handle & 0xFFFFFFFFu);
		if (target == GL_FRAMEBUFFER_DEFAULT) {
			gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
			gl_.DrawBuffer(object);
			return drain_errors(gl_) == GL_NO_ERROR;
		}
		gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_draw_);
		if (target == GL_RENDERBUFFER)
			gl_.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, object);
		else
			gl_.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, target, object, 0);
		gl_.DrawBuffer(GL_COLOR_ATTACHMENT0);
		return framebuffer_ok(GL_DRAW_FRAMEBUFFER);
	}

	bool bind_read_texture(GLuint tex)
	{
		gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_read_);
		gl_.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		gl_.ReadBuffer(GL_COLOR_ATTACHMENT0);
		return framebuffer_ok(GL_READ_FRAMEBUFFER);
	}

	bool bind_draw_texture(GLuint tex)
	{
		gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_draw_);
		gl_.FramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
		gl_.DrawBuffer(GL_COLOR_ATTACHMENT0);
		return framebuffer_ok(GL_DRAW_FRAMEBUFFER);
	}

	void blit(uint32_t w, uint32_t h, bool mirror)
	{
		const GLint gw = static_cast<GLint>(w);
		const GLint gh = static_cast<GLint>(h);
		gl_.BlitFramebuffer(0, 0, gw, gh, 0, mirror ? gh : 0, gw, mirror ? 0 : gh,
			GL_COLOR_BUFFER_BIT, GL_NEAREST);
	}

	BridgeStep copy_in(reshade::api::resource src, const PlaneSlot &dst, uint32_t w, uint32_t h,
		uint32_t samples)
	{
		if (samples > 1) {
			if (!ensure_scratch(resolve_, w, h, dst.internal_format))
				return BridgeStep::Import;
			if (!bind_read_source(src) || !bind_draw_texture(resolve_.tex))
				return BridgeStep::Import;
			blit(w, h, false);
			if (drain_errors(gl_) != GL_NO_ERROR) {
				last_error = L"the game's multisampled colour buffer could not be resolved, "
					L"so it cannot be upscaled. Turning the game's anti-aliasing off is the "
					L"one thing that helps.";
				return BridgeStep::Import;
			}
			if (!bind_read_texture(resolve_.tex) || !bind_draw_texture(dst.tex))
				return BridgeStep::Import;
			blit(w, h, true);
		} else {
			if (!bind_read_source(src) || !bind_draw_texture(dst.tex))
				return BridgeStep::Import;
			blit(w, h, true);
		}
		const GLenum err = drain_errors(gl_);
		if (err != GL_NO_ERROR) {
			last_error = std::wstring(L"the game's frame could not be copied into the shared "
				L"texture (") + gl_error_name(err) + L").";
			return BridgeStep::Import;
		}
		return BridgeStep::Ok;
	}

	BridgeStep copy_in_depth(reshade::api::resource src, const PlaneSlot &dst, uint32_t w, uint32_t h)
	{
		const size_t bytes = static_cast<size_t>(w) * h * 4;
		if (!ensure_pbo(bytes) || !ensure_scratch(depth_scratch_, w, h, GL_R32F))
			return BridgeStep::Import;

		const GLenum target = static_cast<GLenum>(src.handle >> 40);
		const GLuint object = static_cast<GLuint>(src.handle & 0xFFFFFFFFu);
		if (target == GL_FRAMEBUFFER_DEFAULT) {
			gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, 0);
		} else {
			gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_depth_);
			if (target == GL_RENDERBUFFER)
				gl_.FramebufferRenderbuffer(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, object);
			else
				gl_.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, target, object, 0);
			if (!framebuffer_ok(GL_READ_FRAMEBUFFER))
				return BridgeStep::Import;
		}

		gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, pbo_);
		gl_.ReadPixels(0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
			GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
		gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, 0);
		GLenum err = drain_errors(gl_);
		if (err != GL_NO_ERROR) {
			last_error = std::wstring(L"the game's depth buffer could not be read (") +
				gl_error_name(err) + L"); the upscaler runs without depth.";
			return BridgeStep::Import;
		}

		gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo_);
		gl_.BindTexture(GL_TEXTURE_2D, depth_scratch_.tex);
		gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, static_cast<GLsizei>(w), static_cast<GLsizei>(h),
			GL_RED, GL_FLOAT, nullptr);
		gl_.BindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
		err = drain_errors(gl_);
		if (err != GL_NO_ERROR) {
			last_error = std::wstring(L"the game's depth values could not be moved into a "
				L"texture (") + gl_error_name(err) + L").";
			return BridgeStep::Import;
		}

		if (!bind_read_texture(depth_scratch_.tex) || !bind_draw_texture(dst.tex))
			return BridgeStep::Import;
		blit(w, h, true);
		err = drain_errors(gl_);
		if (err != GL_NO_ERROR) {
			last_error = std::wstring(L"the game's depth could not be copied into the shared "
				L"texture (") + gl_error_name(err) + L").";
			return BridgeStep::Import;
		}
		return BridgeStep::Ok;
	}

	BridgeStep copy_out(const PlaneSlot &src, reshade::api::resource dst, uint32_t w, uint32_t h)
	{
		if (!bind_read_texture(src.tex) || !bind_draw_dest(dst))
			return BridgeStep::Export;
		blit(w, h, true);
		const GLenum err = drain_errors(gl_);
		if (err != GL_NO_ERROR) {
			last_error = std::wstring(L"the upscaled frame could not be written back into the "
				L"game's buffer (") + gl_error_name(err) + L").";
			return BridgeStep::Export;
		}
		return BridgeStep::Ok;
	}

	bool engine_copy(ID3D12Resource *tex, uint32_t w, uint32_t h, std::vector<uint32_t> &pixels,
		bool to_texture)
	{
		ID3D12Device *const dev = engine_->device();
		const D3D12_RESOURCE_DESC tex_desc = tex->GetDesc();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
		UINT64 total = 0;
		dev->GetCopyableFootprints(&tex_desc, 0, 1, 0, &fp, nullptr, nullptr, &total);

		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = to_texture ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_READBACK;
		D3D12_RESOURCE_DESC buf{};
		buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buf.Width = total;
		buf.Height = 1;
		buf.DepthOrArraySize = 1;
		buf.MipLevels = 1;
		buf.Format = DXGI_FORMAT_UNKNOWN;
		buf.SampleDesc.Count = 1;
		buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

		ID3D12Resource *staging = nullptr;
		if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf,
				to_texture ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
				nullptr, IID_PPV_ARGS(&staging))) || staging == nullptr)
			return false;

		const size_t row = static_cast<size_t>(w) * 4;
		if (to_texture) {
			void *mapped = nullptr;
			const D3D12_RANGE nothing{ 0, 0 };
			if (FAILED(staging->Map(0, &nothing, &mapped)) || mapped == nullptr) {
				safe_release(staging);
				return false;
			}
			for (uint32_t y = 0; y < h; ++y)
				memcpy(static_cast<uint8_t *>(mapped) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
					reinterpret_cast<const uint8_t *>(pixels.data()) + static_cast<size_t>(y) * row, row);
			staging->Unmap(0, nullptr);
		}

		ID3D12GraphicsCommandList *const cmd = engine_->begin_list();
		if (cmd == nullptr) {
			safe_release(staging);
			return false;
		}
		D3D12_RESOURCE_BARRIER barrier{};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Transition.pResource = tex;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = SharedPlane::kState;
		barrier.Transition.StateAfter = to_texture
			? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_COPY_SOURCE;
		cmd->ResourceBarrier(1, &barrier);

		D3D12_TEXTURE_COPY_LOCATION placed{};
		placed.pResource = staging;
		placed.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		placed.PlacedFootprint = fp;
		D3D12_TEXTURE_COPY_LOCATION whole{};
		whole.pResource = tex;
		whole.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		whole.SubresourceIndex = 0;
		if (to_texture)
			cmd->CopyTextureRegion(&whole, 0, 0, 0, &placed, nullptr);
		else
			cmd->CopyTextureRegion(&placed, 0, 0, 0, &whole, nullptr);

		barrier.Transition.StateBefore = barrier.Transition.StateAfter;
		barrier.Transition.StateAfter = SharedPlane::kState;
		cmd->ResourceBarrier(1, &barrier);

		bool ok = engine_->submit_list() && engine_->wait_cpu(engine_->last_submitted());
		if (ok && !to_texture) {
			void *mapped = nullptr;
			const D3D12_RANGE all{ 0, static_cast<SIZE_T>(total) };
			if (SUCCEEDED(staging->Map(0, &all, &mapped)) && mapped != nullptr) {
				for (uint32_t y = 0; y < h; ++y)
					memcpy(reinterpret_cast<uint8_t *>(pixels.data()) + static_cast<size_t>(y) * row,
						static_cast<const uint8_t *>(mapped) + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch,
						row);
				const D3D12_RANGE nothing{ 0, 0 };
				staging->Unmap(0, &nothing);
			} else {
				ok = false;
			}
		}
		safe_release(staging);
		return ok;
	}

	bool verify_route()
	{
		constexpr uint32_t kN = 64;
		constexpr size_t kPx = static_cast<size_t>(kN) * kN;

		StateGuard guard(gl_);
		PlaneSlot probe{};
		if (!make_plane(probe, kN, kN, DXGI_FORMAT_R8G8B8A8_UNORM, true))
			return false;

		std::vector<uint32_t> pattern(kPx);
		for (uint32_t y = 0; y < kN; ++y)
			for (uint32_t x = 0; x < kN; ++x)
				pattern[static_cast<size_t>(y) * kN + x] =
					0xFF000000u | x | (y << 8) | ((x ^ y) << 16);

		bool ok = false;
		gl_.BindTexture(GL_TEXTURE_2D, probe.tex);
		gl_.TexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, kN, kN, GL_RGBA, GL_UNSIGNED_BYTE, pattern.data());
		gl_.Finish();
		if (drain_errors(gl_) == GL_NO_ERROR) {
			std::vector<uint32_t> readback(kPx, 0xCDCDCDCDu);
			if (engine_copy(probe.shared.engine, kN, kN, readback, false) && readback == pattern) {
				for (uint32_t &v : pattern)
					v ^= 0x00FFFFFFu;
				if (engine_copy(probe.shared.engine, kN, kN, pattern, true)) {
					std::vector<uint32_t> back(kPx, 0xCDCDCDCDu);
					gl_.BindTexture(GL_TEXTURE_2D, probe.tex);
					gl_.GetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, back.data());
					ok = drain_errors(gl_) == GL_NO_ERROR && back == pattern;
				}
			}
		}

		delete_gl_plane(probe.tex, probe.mem);
		bridge_util::release_plane(probe.shared);
		drain_errors(gl_);
		if (!ok)
			last_error = L"this OpenGL driver accepted a shared Direct3D 12 texture but the "
				L"pixels did not survive the round trip, so the two disagree about how it is "
				L"laid out. The frame has to travel through system memory instead.";
		return ok;
	}

	EngineDevice *engine_ = nullptr;
	reshade::api::device *device_ = nullptr;

	HMODULE gl32_ = nullptr;
	PfnWglGetProcAddress wgl_get_proc_ = nullptr;
	PfnWglGetCurrentContext wgl_current_ = nullptr;
	HGLRC gl_context_ = nullptr;
	GlApi gl_{};

	GLuint fbo_read_ = 0;
	GLuint fbo_draw_ = 0;
	GLuint fbo_depth_ = 0;
	GLuint pbo_ = 0;
	size_t pbo_bytes_ = 0;

	PlaneSlot color_{};
	PlaneSlot depth_{};
	PlaneSlot motion_{};
	ScratchTex resolve_{};
	ScratchTex depth_scratch_{};
	std::vector<ReadableSlot> readables_;

	SharedSemaphore to_engine_{};
	SharedSemaphore to_game_{};
	bool gpu_sync_ = false;
	const char *sync_ = kSyncNoExtension;
};

}

FrameBridge *make_bridge_opengl() { return new BridgeOpenGl(); }

}
