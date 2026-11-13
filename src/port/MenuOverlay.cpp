/**
 * @file MenuOverlay.cpp
 * @brief Implementation of the port's menu and start-up hint (see MenuOverlay.h).
 */
#include "MenuOverlay.h"

#include "BitmapFont.h"
#include "Log.h"
#include "RemoteInput.h"
#include "SdlHooks.h"

#include <GLES2/gl2.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace s3w
{
namespace MenuOverlay
{
namespace
{

// ---------------------------------------------------------------------------
// Layout and timing
// ---------------------------------------------------------------------------

/** Logical canvas: divides 1080p and 2160p panels by whole numbers so the 8x8 font stays crisp. */
constexpr int kCanvasWidth  = 480;
constexpr int kCanvasHeight = 270;
/** The start-up hint shows for this long after the game's first frame. */
constexpr Uint32 kHintMs = 15000;
/** The menu asks for fresh information this often while open. */
constexpr Uint32 kInfoRefreshMs = 3000;
/** Longest wait for the controller buttons to be let go before the game resumes. */
constexpr Uint32 kReleaseWaitMs = 1500;

/** An RGBA colour in 0..1. */
struct Rgba {
    float r, g, b, a;
};

constexpr Rgba kPanel     = { 0.04f, 0.07f, 0.20f, 0.92f };
constexpr Rgba kPanelEdge = { 0.95f, 0.75f, 0.15f, 1.0f };
constexpr Rgba kTitle     = { 1.0f, 0.86f, 0.30f, 1.0f };
constexpr Rgba kText      = { 0.95f, 0.95f, 0.92f, 1.0f };
constexpr Rgba kDim       = { 0.65f, 0.72f, 0.85f, 1.0f };
constexpr Rgba kAccent    = { 0.50f, 0.85f, 1.0f, 1.0f };
constexpr Rgba kWarning   = { 1.0f, 0.60f, 0.40f, 1.0f };
constexpr Rgba kSelection = { 0.20f, 0.35f, 0.75f, 1.0f };

/** The menu's choices, in order. */
enum class Choice {
    Resume,
    Restart,
    Exit,
};
constexpr std::array<const char *, 3> kChoiceLabels = { "Resume", "Restart the game", "Exit" };

// ---------------------------------------------------------------------------
// Shared state
// ---------------------------------------------------------------------------

std::mutex g_providerMutex;
std::function<OverlayInfo()> g_provider;
std::atomic<bool> g_openRequested{ false };
std::atomic<bool> g_open{ false };
std::atomic<bool> g_restart{ false };
std::atomic<SDL_AudioDeviceID> g_audioDevice{ 0 };
Uint32 g_firstSwapAt = 0;

OverlayInfo FetchInfo()
{
    std::lock_guard<std::mutex> lock(g_providerMutex);
    return g_provider ? g_provider() : OverlayInfo{};
}

// ---------------------------------------------------------------------------
// GL state the overlay touches, saved before drawing and restored after
// ---------------------------------------------------------------------------

/** One vertex attribute's full setup. */
struct AttribState {
    GLint enabled = 0, size = 4, type = GL_FLOAT, normalized = 0, stride = 0, buffer = 0;
    void *pointer = nullptr;
};

struct GlState {
    GLint program = 0, arrayBuffer = 0, activeTexture = GL_TEXTURE0, texture = 0, framebuffer = 0;
    GLint viewport[4] = {};
    GLboolean blend = GL_FALSE, scissor = GL_FALSE, depth = GL_FALSE, cull = GL_FALSE, stencil = GL_FALSE;
    GLint blendSrcRgb = GL_ONE, blendDstRgb = GL_ZERO, blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    GLint blendEquationRgb = GL_FUNC_ADD, blendEquationAlpha = GL_FUNC_ADD;
    GLboolean colorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    GLint unpackAlignment = 4;
    AttribState attribs[2];

    void Save()
    {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &arrayBuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
        glActiveTexture(GL_TEXTURE0);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_VIEWPORT, viewport);
        blend   = glIsEnabled(GL_BLEND);
        scissor = glIsEnabled(GL_SCISSOR_TEST);
        depth   = glIsEnabled(GL_DEPTH_TEST);
        cull    = glIsEnabled(GL_CULL_FACE);
        stencil = glIsEnabled(GL_STENCIL_TEST);
        glGetIntegerv(GL_BLEND_SRC_RGB, &blendSrcRgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &blendDstRgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &blendSrcAlpha);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &blendDstAlpha);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);
        glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
        for (GLuint index = 0; index < 2; ++index) {
            AttribState &attrib = attribs[index];
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &attrib.enabled);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &attrib.size);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &attrib.type);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &attrib.normalized);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &attrib.stride);
            glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &attrib.buffer);
            glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &attrib.pointer);
        }
    }

    void Restore() const
    {
        for (GLuint index = 0; index < 2; ++index) {
            const AttribState &attrib = attribs[index];
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(attrib.buffer));
            glVertexAttribPointer(index, attrib.size, static_cast<GLenum>(attrib.type),
                                  attrib.normalized ? GL_TRUE : GL_FALSE, attrib.stride, attrib.pointer);
            if (attrib.enabled)
                glEnableVertexAttribArray(index);
            else
                glDisableVertexAttribArray(index);
        }
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(arrayBuffer));
        glUseProgram(static_cast<GLuint>(program));
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
        glActiveTexture(static_cast<GLenum>(activeTexture));
        glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        const auto setEnabled = [](GLenum capability, GLboolean on) { on ? glEnable(capability) : glDisable(capability); };
        setEnabled(GL_BLEND, blend);
        setEnabled(GL_SCISSOR_TEST, scissor);
        setEnabled(GL_DEPTH_TEST, depth);
        setEnabled(GL_CULL_FACE, cull);
        setEnabled(GL_STENCIL_TEST, stencil);
        glBlendFuncSeparate(static_cast<GLenum>(blendSrcRgb), static_cast<GLenum>(blendDstRgb),
                            static_cast<GLenum>(blendSrcAlpha), static_cast<GLenum>(blendDstAlpha));
        glBlendEquationSeparate(static_cast<GLenum>(blendEquationRgb), static_cast<GLenum>(blendEquationAlpha));
        glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    }
};

// ---------------------------------------------------------------------------
// Drawing: solid rectangles, 8x8 text and the paused game frame
// ---------------------------------------------------------------------------

constexpr const char *kVertexSource = R"(
attribute vec4 a_vertex; // xy = pixels from the top left, zw = texture coordinate
uniform vec2 u_screen;
varying vec2 v_uv;
void main() {
    v_uv = a_vertex.zw;
    vec2 ndc = a_vertex.xy / u_screen * 2.0 - 1.0;
    gl_Position = vec4(ndc.x, -ndc.y, 0.0, 1.0);
}
)";

// u_mode: 0 = solid colour, 1 = glyph coverage (the font atlas), 2 = image tinted by u_color.
constexpr const char *kFragmentSource = R"(
precision mediump float;
uniform sampler2D u_texture;
uniform vec4 u_color;
uniform int u_mode;
varying vec2 v_uv;
void main() {
    if (u_mode == 0) {
        gl_FragColor = u_color;
    } else if (u_mode == 1) {
        gl_FragColor = vec4(u_color.rgb, u_color.a * texture2D(u_texture, v_uv).r);
    } else {
        gl_FragColor = vec4(texture2D(u_texture, v_uv).rgb * u_color.rgb, 1.0);
    }
}
)";

class Painter
{
public:
    /** Creates the shader and font texture on first use. @return False if GL refused. */
    bool Ready()
    {
        if (program_ || failed_)
            return program_ != 0;
        program_ = Link();
        if (!program_) {
            failed_ = true;
            return false;
        }
        locScreen_  = glGetUniformLocation(program_, "u_screen");
        locColor_   = glGetUniformLocation(program_, "u_color");
        locMode_    = glGetUniformLocation(program_, "u_mode");
        locTexture_ = glGetUniformLocation(program_, "u_texture");
        CreateFont();
        return true;
    }

    /** Starts a frame of size @p width x @p height pixels; the state must have been saved. */
    void Begin(int width, int height)
    {
        width_  = width;
        height_ = height;
        scale_  = std::max(1, std::min(width / kCanvasWidth, height / kCanvasHeight));
        originX_ = (width - kCanvasWidth * scale_) / 2;
        originY_ = (height - kCanvasHeight * scale_) / 2;

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, width, height);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_CULL_FACE);
        glDisable(GL_STENCIL_TEST);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glUseProgram(program_);
        glUniform2f(locScreen_, static_cast<float>(width), static_cast<float>(height));
        glUniform1i(locTexture_, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindBuffer(GL_ARRAY_BUFFER, 0); // client-side vertex arrays
        glDisableVertexAttribArray(1);
        glEnableVertexAttribArray(0);
    }

    /** Copies the frame the game just drew into a texture, to show behind the menu. */
    void CaptureFrame()
    {
        if (!frame_)
            glGenTextures(1, &frame_);
        glBindTexture(GL_TEXTURE_2D, frame_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glCopyTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 0, 0, width_, height_, 0);
    }

    /** Draws the captured frame over the whole screen, darkened. */
    void DrawFrame()
    {
        if (!frame_)
            return;
        glBindTexture(GL_TEXTURE_2D, frame_);
        // The copy's first row is the bottom of the screen.
        Quad(0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f, 1.0f, 0.0f, 2, { 0.35f, 0.35f, 0.4f, 1.0f });
    }

    /** Fills a rectangle given in canvas units. */
    void FillRect(int x, int y, int w, int h, Rgba color)
    {
        Quad(ToX(x), ToY(y), ToX(x + w), ToY(y + h), 0, 0, 0, 0, 0, color);
    }

    /** Draws @p text with its top-left corner at canvas (@p x, @p y), @p size times the 8x8 font. */
    void Text(int x, int y, const std::string &text, int size, Rgba color)
    {
        if (text.empty())
            return;
        vertices_.clear();
        const float glyph = static_cast<float>(BitmapFont::kGlyphSize * size * scale_);
        float penX        = ToX(x);
        const float top   = ToY(y);
        for (const char c : text) {
            int index = static_cast<unsigned char>(c) - BitmapFont::kFirstChar;
            if (index < 0 || index >= BitmapFont::kGlyphCount)
                index = '?' - BitmapFont::kFirstChar;
            const float u0 = static_cast<float>(index) / BitmapFont::kGlyphCount;
            const float u1 = static_cast<float>(index + 1) / BitmapFont::kGlyphCount;
            AppendQuad(penX, top, penX + glyph, top + glyph, u0, 0.0f, u1, 1.0f);
            penX += glyph;
        }
        glBindTexture(GL_TEXTURE_2D, font_);
        Flush(1, color);
    }

    /** Draws @p text centred on canvas column @p centerX. */
    void TextCentered(int centerX, int y, const std::string &text, int size, Rgba color)
    {
        Text(centerX - Width(text, size) / 2, y, text, size, color);
    }

    static int Width(const std::string &text, int size) { return static_cast<int>(text.size()) * BitmapFont::kGlyphSize * size; }

private:
    float ToX(int x) const { return static_cast<float>(originX_ + x * scale_); }
    float ToY(int y) const { return static_cast<float>(originY_ + y * scale_); }

    void AppendQuad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1)
    {
        const std::array<float, 24> quad = { x0, y0, u0, v0, x1, y0, u1, v0, x1, y1, u1, v1,
                                             x0, y0, u0, v0, x1, y1, u1, v1, x0, y1, u0, v1 };
        vertices_.insert(vertices_.end(), quad.begin(), quad.end());
    }

    void Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, int mode, Rgba color)
    {
        vertices_.clear();
        AppendQuad(x0, y0, x1, y1, u0, v0, u1, v1);
        Flush(mode, color);
    }

    void Flush(int mode, Rgba color)
    {
        glUniform1i(locMode_, mode);
        glUniform4f(locColor_, color.r, color.g, color.b, color.a);
        glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 4 * sizeof(float), vertices_.data());
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertices_.size() / 4));
    }

    static GLuint Compile(GLenum type, const char *source)
    {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            std::array<char, 512> log{};
            glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
            Log::Printf("menu shader: %s", log.data());
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }

    static GLuint Link()
    {
        const GLuint vertex   = Compile(GL_VERTEX_SHADER, kVertexSource);
        const GLuint fragment = Compile(GL_FRAGMENT_SHADER, kFragmentSource);
        if (!vertex || !fragment) {
            glDeleteShader(vertex);
            glDeleteShader(fragment);
            return 0;
        }
        const GLuint program = glCreateProgram();
        glAttachShader(program, vertex);
        glAttachShader(program, fragment);
        glBindAttribLocation(program, 0, "a_vertex");
        glLinkProgram(program);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok) {
            glDeleteProgram(program);
            return 0;
        }
        return program;
    }

    void CreateFont()
    {
        constexpr int kAtlasWidth  = BitmapFont::kGlyphCount * BitmapFont::kGlyphSize;
        constexpr int kAtlasHeight = BitmapFont::kGlyphSize;
        std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kAtlasWidth * kAtlasHeight), 0);
        for (int glyph = 0; glyph < BitmapFont::kGlyphCount; ++glyph) {
            for (int row = 0; row < BitmapFont::kGlyphSize; ++row) {
                const std::uint8_t bits = BitmapFont::kGlyphs[static_cast<std::size_t>(glyph)][static_cast<std::size_t>(row)];
                for (int col = 0; col < BitmapFont::kGlyphSize; ++col) {
                    if (bits & (1u << col))
                        pixels[static_cast<std::size_t>(row * kAtlasWidth + glyph * BitmapFont::kGlyphSize + col)] = 255;
                }
            }
        }
        glGenTextures(1, &font_);
        glBindTexture(GL_TEXTURE_2D, font_);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, kAtlasWidth, kAtlasHeight, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, pixels.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    GLuint program_ = 0, font_ = 0, frame_ = 0;
    bool failed_ = false;
    GLint locScreen_ = -1, locColor_ = -1, locMode_ = -1, locTexture_ = -1;
    int width_ = 0, height_ = 0, scale_ = 1, originX_ = 0, originY_ = 0;
    std::vector<float> vertices_;
};

Painter &ThePainter()
{
    static Painter painter;
    return painter;
}

// ---------------------------------------------------------------------------
// The start-up hint and the menu
// ---------------------------------------------------------------------------

void DrawHint(Painter &painter, const OverlayInfo &info)
{
    const std::string where = info.address.empty() ? std::string("Transfer page: connect the TV to a network")
                                                   : "Transfer page: " + info.address + "  PIN " + info.pin;
    const std::string line  = "BACK: menu   " + where;
    const int width         = std::min(kCanvasWidth - 8, Painter::Width(line, 1) + 12);
    painter.FillRect((kCanvasWidth - width) / 2, kCanvasHeight - 22, width, 14, kPanel);
    painter.TextCentered(kCanvasWidth / 2, kCanvasHeight - 19, line, 1, kText);
}

/** Splits @p text into lines of at most @p maxChars characters, at spaces where possible. */
std::vector<std::string> WrapText(const std::string &text, std::size_t maxChars)
{
    std::vector<std::string> lines;
    std::string rest = text;
    while (rest.size() > maxChars) {
        std::size_t cut = rest.rfind(' ', maxChars);
        if (cut == std::string::npos || cut == 0)
            cut = maxChars; // one long word (a URL): break it anywhere
        lines.push_back(rest.substr(0, cut));
        rest = rest.substr(cut);
        while (!rest.empty() && rest.front() == ' ')
            rest.erase(rest.begin());
    }
    if (!rest.empty())
        lines.push_back(rest);
    return lines;
}

void DrawMenu(Painter &painter, const OverlayInfo &info, int selected)
{
    constexpr int kLeft = 6, kWidth = kCanvasWidth - 2 * kLeft, kTop = 6, kHeight = kCanvasHeight - 2 * kTop;
    constexpr int kLine = 10;
    constexpr int kTextX = kLeft + 8;
    constexpr std::size_t kMaxChars = static_cast<std::size_t>((kWidth - 16) / BitmapFont::kGlyphSize);
    painter.DrawFrame();
    painter.FillRect(kLeft - 2, kTop - 2, kWidth + 4, kHeight + 4, kPanelEdge);
    painter.FillRect(kLeft, kTop, kWidth, kHeight, kPanel);

    const int center = kCanvasWidth / 2;
    int y            = kTop + 6;
    painter.TextCentered(center, y, "SONIC 3 A.I.R.", 2, kTitle);
    y += 19;
    painter.TextCentered(center, y, std::string("for webOS - game v") + S3W_AIR_VERSION + ", port v" + S3W_VERSION, 1, kDim);
    y += 14;

    for (std::size_t index = 0; index < kChoiceLabels.size(); ++index) {
        const bool current = static_cast<int>(index) == selected;
        if (current)
            painter.FillRect(center - 90, y - 3, 180, 13, kSelection);
        painter.TextCentered(center, y, kChoiceLabels[index], 1, current ? kTitle : kText);
        y += 14;
    }
    y += 2;

    // Information lines, wrapped to the panel.
    const auto line = [&](const std::string &text, Rgba color, int indent = 0) {
        for (const std::string &part : WrapText(text, kMaxChars - static_cast<std::size_t>(indent))) {
            painter.Text(kTextX + indent * BitmapFont::kGlyphSize, y, part, 1, color);
            y += kLine;
        }
    };
    if (info.address.empty()) {
        line("Transfer page: connect the TV to a network.", kWarning);
    }
    else {
        line("Transfer page (ROM, saves, mods, soundtrack):", kText);
        line(info.address + "   PIN " + info.pin, kAccent, 2);
    }
    y += 3;
    line("ROM: " + info.romSource, kDim);
    line("Remastered soundtrack: " + info.soundtrackSource, kDim);
    line("Mods: " + info.modsSource, kDim);
    line("Root access: " + info.root, kDim);
    y += 3;
    if (info.restartPending) {
        line("New files arrived: restart the game to use them.", kWarning);
        y += 3;
    }
    line("Remote held sideways: 2 8 6 4 = D-pad, 0 or OK = Start, green A, red B, blue X, yellow Y. "
         "Controllers: Guide, or Back + Start, open this menu.", kDim);
    y += 3;
    line(S3W_PROJECT_URL, kAccent);

    painter.TextCentered(center, kTop + kHeight - 11, "OK: choose   BACK: resume", 1, kDim);
}

/** Whether any face, Start or Back button is still held on an open game controller. */
bool AnyControllerButtonHeld()
{
    for (int index = 0; index < SDL_NumJoysticks(); ++index) {
        if (!SDL_IsGameController(index))
            continue;
        SDL_GameController *controller = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(index));
        if (!controller)
            continue;
        for (const SDL_GameControllerButton button :
             { SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
               SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_GUIDE }) {
            if (SDL_GameControllerGetButton(controller, button))
                return true;
        }
    }
    return false;
}

/** Runs the menu until it closes. @return The choice made. */
Choice RunMenu(SDL_Window *window, void (*swap)(SDL_Window *), int width, int height)
{
    Painter &painter = ThePainter();
    OverlayInfo info = FetchInfo();
    Uint32 nextInfo  = SDL_GetTicks() + kInfoRefreshMs;
    int selected     = 0;

    // The frame the game just drew stays behind the menu.
    painter.Begin(width, height);
    painter.CaptureFrame();

    bool running  = true;
    Choice choice = Choice::Resume;
    while (running) {
        SDL_Event event;
        while (SdlHooks::PollRealEvent(&event)) {
            SdlHooks::HandleDeviceEvent(event);
            if (event.type == SDL_QUIT) {
                choice  = Choice::Exit;
                running = false;
                break;
            }
            switch (RemoteInput::ToMenuAction(event)) {
                case MenuAction::Up:
                    selected = (selected + static_cast<int>(kChoiceLabels.size()) - 1) % static_cast<int>(kChoiceLabels.size());
                    break;
                case MenuAction::Down: selected = (selected + 1) % static_cast<int>(kChoiceLabels.size()); break;
                case MenuAction::Confirm:
                    choice  = static_cast<Choice>(selected);
                    running = false;
                    break;
                case MenuAction::Cancel:
                    choice  = Choice::Resume;
                    running = false;
                    break;
                case MenuAction::None: break;
            }
            if (!running)
                break;
        }

        const Uint32 now = SDL_GetTicks();
        if (SDL_TICKS_PASSED(now, nextInfo)) {
            info     = FetchInfo();
            nextInfo = now + kInfoRefreshMs;
        }
        painter.Begin(width, height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        DrawMenu(painter, info, selected);
        swap(window);
        SDL_Delay(4); // in case the swap does not wait for the display
    }

    // The button that closed the menu must not reach the game as a fresh press.
    const Uint32 giveUpAt = SDL_GetTicks() + kReleaseWaitMs;
    while (AnyControllerButtonHeld() && !SDL_TICKS_PASSED(SDL_GetTicks(), giveUpAt)) {
        SDL_Event event;
        while (SdlHooks::PollRealEvent(&event))
            SdlHooks::HandleDeviceEvent(event);
        SDL_Delay(10);
    }
    // Leave the paused frame on screen for the game's next swap.
    painter.Begin(width, height);
    painter.DrawFrame();
    return choice;
}

} // namespace

void SetInfoProvider(std::function<OverlayInfo()> provider)
{
    std::lock_guard<std::mutex> lock(g_providerMutex);
    g_provider = std::move(provider);
}

void RequestOpen() { g_openRequested.store(true); }

bool IsOpen() { return g_open.load(); }

void SetAudioDevice(SDL_AudioDeviceID device) { g_audioDevice.store(device); }

bool RestartRequested() { return g_restart.load(); }

void RequestRestart()
{
    g_restart.store(true);
    SDL_Event quit{};
    quit.type = SDL_QUIT;
    SDL_PushEvent(&quit);
}

void OnSwap(SDL_Window *window, void (*swap)(SDL_Window *))
{
    const Uint32 now = SDL_GetTicks();
    if (g_firstSwapAt == 0)
        g_firstSwapAt = now == 0 ? 1 : now;
    const bool hint = !SDL_TICKS_PASSED(now, g_firstSwapAt + kHintMs);
    const bool menu = g_openRequested.exchange(false);
    if (!hint && !menu)
        return;

    Painter &painter = ThePainter();
    int width = 0, height = 0;
    SDL_GL_GetDrawableSize(window, &width, &height);
    if (width <= 0 || height <= 0)
        return;

    GlState saved;
    saved.Save();
    if (painter.Ready()) {
        if (menu) {
            // The game stands still while the menu runs here, inside its own swap.
            const SDL_AudioDeviceID audio = g_audioDevice.load();
            const bool wasPlaying         = audio != 0 && SDL_GetAudioDeviceStatus(audio) == SDL_AUDIO_PLAYING;
            if (wasPlaying)
                SDL_PauseAudioDevice(audio, 1);
            g_open.store(true);
            g_firstSwapAt = 1; // the hint has done its job once the menu was found
            const Choice choice = RunMenu(window, swap, width, height);
            g_open.store(false);
            if (wasPlaying)
                SDL_PauseAudioDevice(audio, 0);

            if (choice == Choice::Restart) {
                RequestRestart();
            }
            else if (choice == Choice::Exit) {
                SDL_Event quit{};
                quit.type = SDL_QUIT;
                SDL_PushEvent(&quit);
            }
        }
        else {
            // The hint's text changes rarely (the address); look it up every few seconds, not every frame.
            static OverlayInfo hintInfo;
            static Uint32 hintInfoAt = 0;
            if (hintInfoAt == 0 || SDL_TICKS_PASSED(now, hintInfoAt + kInfoRefreshMs)) {
                hintInfo   = FetchInfo();
                hintInfoAt = now == 0 ? 1 : now;
            }
            painter.Begin(width, height);
            DrawHint(painter, hintInfo);
        }
    }
    saved.Restore();
}

} // namespace MenuOverlay
} // namespace s3w
