#include <SDL.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <tuple>
#include <vector>

namespace {

constexpr int kWidth = 384;
constexpr int kHeight = 400;
constexpr int kTickMs = 25; // The original default is 40 fps.
constexpr int32_t kOne = 1 << 16;

enum class Screen { Title, Playing, GameOver, Clear };
enum class EntityKind {
  None, Enemy1, Enemy2, Gates1, Gates2, EnemyBullet, EnemyBullet90,
  PlayerShot, Effect, Particle
};
enum class EffectCallback { None, NAnime, TrailEmitter, ExplosionEmitter, RectEmitter, RadialEmitter };

constexpr uint32_t kRuntimeBase = 0x00416000;
constexpr uint32_t kScriptGatesMode1 = 0x00416488;
constexpr uint32_t kScriptGatesMode2 = 0x004164a8;
constexpr uint32_t kScriptBulletHit = 0x00416538;
constexpr uint32_t kScriptBomb = 0x00416598;
constexpr uint32_t kScriptTrail = 0x004165d8;
constexpr uint32_t kScriptSpark = 0x00416640;
constexpr uint32_t kScriptExplosion = 0x00416690;
constexpr uint32_t kScriptGatesDeath = 0x004166d0;
constexpr uint32_t kScriptShotHit = 0x00416708;
constexpr uint32_t kScriptMuzzle = 0x00416728;

struct SpriteInfo {
  int32_t exists = 0;
  int32_t atlasX = 0;
  int32_t atlasY = 0;
  int32_t width = 0;
  int32_t height = 0;
  int32_t offsetX = 0;
  int32_t offsetY = 0;
  int32_t collisionRight = 0;
  int32_t collisionBottom = 0;
};

struct Entity {
  bool active = false;
  EntityKind kind = EntityKind::None;
  int32_t x = 0;
  int32_t y = 0;
  int32_t vx = 0;
  int32_t vy = 0;
  int sprite = -1;
  int hp = 0;
  int initialHp = 0;
  // The original shared enemy record overlays type-specific data at +0x30.
  // In particular, Gates HP is a word while Enemy2 steering is its low byte.
  int32_t slotData30 = 0;
  int timer = 0;
  int phase = 0;
  int mode = 0;
  int angle = 0;
  int steerAngle = 0;
  int aux = 0;
  int cycle = 0;
  int ttl = 0;
  int frameDelay = 0;
  int frameFirst = 0;
  int frameLast = 0;
  uint32_t scriptPc = 0;
  int animeDelay = 0;
  EffectCallback callback = EffectCallback::None;
  bool hard = false;
  bool bounce = false;
  bool piercing = false;
  bool modeLocked = false;
  bool collidable = true;
  int32_t savedVx = 0;
  int32_t savedVy = 0;
};

struct AudioClip {
  std::vector<float> samples; // Interleaved stereo, converted to the device rate.
};

struct Voice {
  const AudioClip* clip = nullptr;
  size_t position = 0;
};

struct ReplayHeader {
  uint32_t seed = 1;
  uint32_t randomIndex = 0;
  uint32_t length = 0;
  uint32_t modeFlags = 0;
};

// gates32.sco is ten of these records written verbatim by FUN_00409010/
// FUN_00409050.  FUN_00408f60 fills the four integers and leaves the optional
// name empty in this game.
struct ScoreEntry {
  int32_t score = 0;
  int32_t time = 0;
  int32_t level = 0;
  int32_t flags = 0;
  std::array<char, 16> name{};
};
static_assert(sizeof(ScoreEntry) == 0x20);

class Gates32 {
 public:
  bool initialize();
  void shutdown();
  void browserFrame();

 private:
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* titleTexture_ = nullptr;
  SDL_Texture* titleBackgroundTexture_ = nullptr;
  SDL_Texture* backTexture_ = nullptr;
  SDL_Texture* earthTexture_ = nullptr;
  SDL_Texture* spriteTexture_ = nullptr;
  SDL_Texture* font8Texture_ = nullptr;
  SDL_Texture* font16Texture_ = nullptr;
  SDL_AudioDeviceID audioDevice_ = 0;
  SDL_AudioSpec audioSpec_{};
  std::array<AudioClip, 10> clips_{};
  std::array<Voice, 32> voices_{};
  std::array<SpriteInfo, 1536> sprites_{};
  std::array<int8_t, 256> atanLut_{};
  std::array<int16_t, 256> sin16_{};
  std::array<int16_t, 256> cos16_{};
  std::array<int32_t, 256> sin32_{};
  std::array<int32_t, 256> cos32_{};
  std::array<int16_t, 1024> randomTable_{};
  std::vector<uint8_t> runtimeCommon_;

  std::vector<Entity> enemies_ = std::vector<Entity>(32);
  std::vector<Entity> enemyBullets_ = std::vector<Entity>(320);
  std::vector<Entity> playerShots_ = std::vector<Entity>(32);
  std::vector<Entity> effects_ = std::vector<Entity>(256);
  std::vector<Entity> bombEffects_ = std::vector<Entity>(80);
  std::vector<Entity> particles_ = std::vector<Entity>(128);
  std::vector<std::array<int, 2>> enemyCachedBottomRight_ =
      std::vector<std::array<int, 2>>(32);
  std::array<ScoreEntry, 10> ranking_{};
  size_t worldCapacity_ = 320;
  size_t enemyManagerCount_ = 0;
  size_t enemyBulletManagerCount_ = 0;
  size_t effectManagerCount_ = 0;
  size_t bombEffectManagerCount_ = 0;
  size_t particleManagerCount_ = 0;
  Entity effectOverflow_{};

  Screen screen_ = Screen::Title;
  bool running_ = true;
  bool backgroundEnabled_ = true;
  bool soundEnabled_ = true;
  bool explosionMode_ = true;
  bool acchohMode_ = false;
  bool keyFire_ = false;
  bool clickFire_ = false;
  int tickMs_ = kTickMs;
  uint32_t lastBrowserTime_ = 0;
  uint32_t accumulator_ = 0;
  uint64_t frameNumber_ = 0;

  int32_t playerX_ = 192 * kOne;
  int32_t playerY_ = 200 * kOne;
  int lives_ = 2;
  int invulnerable_ = 0;
  int shotCooldown_ = 0;
  bool piercingShotsEnabled_ = false;
  int score_ = 0;
  int highScore_ = 0;
  int nextOneUp_ = 100000;
  int deflation_ = 0;
  int level_ = 1;
  int endTimer_ = 0;
  uint64_t endStartFrame_ = 0;
  bool scoreSubmitted_ = false;
  bool titleBackgroundReady_ = false;
  bool captureTitleBackground_ = false;
  int titleTickAccumulator_ = 0;
  int titleIdleCounter_ = 0;
  uint8_t titlePreviousInput_ = 0;
  bool replayAbortRequested_ = false;

  int stageState_ = 0;
  int stageCounter_ = 0;
  int stageCountdown_ = 0;
  int stagePeriod_ = 1;

  uint32_t randomSeed_ = 1;
  int randomIndex_ = 0;
  bool replayPlayback_ = false;
  bool replayRecording_ = false;
  bool replaySession_ = false;
  bool traceActive_ = false;
  bool publishTraceAfterFrame_ = false;
  bool traceCompact_ = false;
  bool traceEndStates_ = false;
  uint64_t traceFrameStart_ = 1;
  uint64_t traceFrameLimit_ = 0;
  size_t replayPosition_ = 0;
  ReplayHeader replayHeader_{};
  std::vector<uint8_t> replayInput_;
  std::string stateTrace_;

  static void audioCallback(void* userdata, Uint8* stream, int len);
  bool loadImages();
  bool loadSpriteInfo();
  bool loadMathTables();
  bool loadAudio();
  SDL_Texture* loadTexture(const char* path, bool colorKey);
  void playSound(int id);

  void handleEvents();
  void update();
  void updateTitle();
  void startGame(uint32_t seed, int randomIndex = 0);
  void startLiveGame();
  void startBundledReplay();
  void startPreviousReplay();
  void startReplay(bool reloadBundled);
  void finishReplayRecording();
  void configurePools();
  void resetObjects();
  uint8_t readLiveInputMask() const;
  uint8_t readInputMask();
  void updatePlayer(uint8_t input);
  void updateStage();
  void updateEnemies();
  void updateEnemyBullets();
  void updatePlayerShots();
  void updateEffects(bool bombPool = false);
  void updateParticles();
  bool advanceNAnime(Entity& entity, int elapsed = 1);
  void dispatchAnimeEvent(Entity& entity, int event, int argument);
  int16_t runtimeI16(uint32_t address) const;
  int32_t runtimeI32(uint32_t address) const;

  Entity* allocate(std::vector<Entity>& pool, EntityKind kind);
  Entity* allocateWorld(std::vector<Entity>& pool, EntityKind kind);
  Entity* spawnEnemy(EntityKind kind, int x, int y, int angle, double speed);
  Entity* spawnEnemyBullet(int x, int y, int angle, double speed,
                           int spriteBase = 192,
                           EntityKind kind = EntityKind::EnemyBullet);
  void spawnPlayerShot();
  void spawnEffect(int x, int y, int first, int last, int delay = 4,
                   int32_t vx = 0, int32_t vy = 0);
  Entity* spawnAnime(int x, int y, uint32_t script, int32_t vx = 0,
                     int32_t vy = 0, int initialDelay = 0, int sprite = -1);
  void spawnParticles(int x, int y, int count);
  void spawnTrailEmitter(int x, int y, int32_t vx, int32_t vy, int timer = 32);
  void spawnExplosionEmitter(int x, int y, int timer, uint32_t script = 0,
                             int32_t vx = 0, int32_t vy = 0,
                             bool assignVelocity = false);
  void spawnRectEmitter(int x, int y, int width, int height, int timer);
  void spawnRadialEmitter(int x, int y, int speedX, int speedY, int count,
                          uint32_t script);
  void spawnBombMuzzle(int x, int y);
  void spawnBomb();
  void hitEnemy(Entity& enemy, const Entity* impact = nullptr, int damage = 1);
  void killEnemy(Entity& enemy);
  void hitEnemyBullet(Entity& bullet);
  void addScore(int base);
  void loadRanking();
  void submitScore(bool cleared);
  void saveRanking();

  void render();
  void renderTitle();
  void renderGame();
  void captureTitleBackground();
  void drawSprite(int sprite, int x, int y);
  void drawText8(int x, int y, const std::string& text);
  void drawText16(int x, int y, const std::string& text);
  void drawText(SDL_Texture* texture, int cellHeight, int x, int y, const std::string& text);
  bool outsideSpriteBounds(const Entity& entity) const;
  SDL_Rect collisionRect(const Entity& entity) const;
  bool intersects(const Entity& a, const Entity& b) const;
  bool intersectsPlayer(const Entity& entity) const;

  int randomSigned();
  int aimAngle(int dx, int dy) const;
  int32_t velocityX16(int angle, double speed) const;
  int32_t velocityY16(int angle, double speed) const;
  int32_t velocityX32(int angle, double speed) const;
  int32_t velocityY32(int angle, double speed) const;
  static int fixedToInt(int32_t value) { return value >> 16; }
  static int32_t withIntegerPart(int32_t value, int integer) {
    return static_cast<int32_t>((static_cast<uint32_t>(value) & 0xffffu) |
                                (static_cast<uint32_t>(static_cast<uint16_t>(integer)) << 16));
  }
  static int clampAngle(int angle) { return angle & 255; }
  static uint32_t currentSeed();
  void loadBundledReplay();
  void saveReplay();
  uint64_t stateHash(uint8_t input) const;
  void appendStateTrace(uint8_t input);
  void publishStateTrace();
  void saveStateTrace();
};

uint32_t Gates32::currentSeed() {
  uint32_t value = static_cast<uint32_t>(SDL_GetTicks());
  return value == 0 ? 1 : value;
}

SDL_Texture* Gates32::loadTexture(const char* path, bool colorKey) {
  SDL_Surface* surface = SDL_LoadBMP(path);
  if (!surface) {
    SDL_Log("SDL_LoadBMP(%s): %s", path, SDL_GetError());
    return nullptr;
  }
  if (colorKey) {
    const Uint8* row = static_cast<const Uint8*>(surface->pixels);
    Uint32 key = 0;
    std::memcpy(&key, row, std::min<int>(surface->format->BytesPerPixel, 4));
    SDL_SetColorKey(surface, SDL_TRUE, key);
  }
  SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer_, surface);
  SDL_FreeSurface(surface);
  if (texture) SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
  return texture;
}

bool Gates32::loadImages() {
  // The DirectDraw version uses the green first palette entry as a color key.
  titleTexture_ = loadTexture("/assets/original/title.bmp", true);
  spriteTexture_ = loadTexture("/assets/runtime/sprites.bmp", true);
  // FUN_00408c90/FUN_00408d50 copy every byte in each 8-pixel glyph row.
  // Unlike sprites, palette index zero is therefore an opaque text backdrop.
  font8Texture_ = loadTexture("/assets/original/string.bmp", false);
  font16Texture_ = loadTexture("/assets/original/string16.bmp", false);

  SDL_Surface* back = SDL_LoadBMP("/assets/original/back.bmp");
  if (back) {
    backTexture_ = SDL_CreateTextureFromSurface(renderer_, back);
    const Uint8* row = static_cast<const Uint8*>(back->pixels);
    Uint32 key = 0;
    std::memcpy(&key, row, std::min<int>(back->format->BytesPerPixel, 4));
    SDL_SetColorKey(back, SDL_TRUE, key);
    earthTexture_ = SDL_CreateTextureFromSurface(renderer_, back);
    SDL_FreeSurface(back);
    if (earthTexture_) SDL_SetTextureBlendMode(earthTexture_, SDL_BLENDMODE_BLEND);
  }
  return titleTexture_ && spriteTexture_ && font8Texture_ && font16Texture_ &&
         backTexture_ && earthTexture_;
}

bool Gates32::loadSpriteInfo() {
  std::ifstream input("/assets/runtime/sprites.dat", std::ios::binary);
  if (!input) return false;
  input.read(reinterpret_cast<char*>(sprites_.data()),
             static_cast<std::streamsize>(sprites_.size() * sizeof(SpriteInfo)));
  return input.good() || input.gcount() == static_cast<std::streamsize>(sprites_.size() * sizeof(SpriteInfo));
}

bool Gates32::loadMathTables() {
  // These ranges were dumped from the initialized data section. Loading them
  // avoids differences between x87 rounding in the 1998 executable and wasm.
  std::ifstream input("/assets/runtime/runtime-common.bin", std::ios::binary);
  if (!input) return false;
  runtimeCommon_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (runtimeCommon_.size() < 0x3ef0) return false;
  std::memcpy(sin16_.data(), runtimeCommon_.data() + 0x35f0, 512);
  std::memcpy(cos16_.data(), runtimeCommon_.data() + 0x3670, 512);
  std::memcpy(atanLut_.data(), runtimeCommon_.data() + 0x38f0, 256);
  std::memcpy(sin32_.data(), runtimeCommon_.data() + 0x39f0, 1024);
  std::memcpy(cos32_.data(), runtimeCommon_.data() + 0x3af0, 1024);
  return true;
}

void Gates32::audioCallback(void* userdata, Uint8* stream, int len) {
  auto* game = static_cast<Gates32*>(userdata);
  auto* output = reinterpret_cast<float*>(stream);
  const size_t count = static_cast<size_t>(len) / sizeof(float);
  std::fill(output, output + count, 0.0f);
  for (Voice& voice : game->voices_) {
    if (!voice.clip) continue;
    const size_t left = voice.clip->samples.size() - voice.position;
    const size_t mixing = std::min(count, left);
    for (size_t i = 0; i < mixing; ++i) output[i] += voice.clip->samples[voice.position + i] * 0.32f;
    voice.position += mixing;
    if (voice.position >= voice.clip->samples.size()) voice = {};
  }
  for (size_t i = 0; i < count; ++i) output[i] = std::clamp(output[i], -1.0f, 1.0f);
}

bool Gates32::loadAudio() {
  SDL_AudioSpec desired{};
  desired.freq = 48000;
  desired.format = AUDIO_F32SYS;
  desired.channels = 2;
  desired.samples = 1024;
  desired.callback = audioCallback;
  desired.userdata = this;
  audioDevice_ = SDL_OpenAudioDevice(nullptr, 0, &desired, &audioSpec_,
                                     SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
  if (!audioDevice_) return false;

  const std::array<const char*, 10> names = {
      "fmake14.wav", "dg06.wav", "rai03_.wav", "bom1.wav", "bom2.wav",
      "fmake04.wav", "fmake18_.wav", "accent1.wav", "fpcm14.wav", "bomber.wav"};
  for (size_t i = 0; i < names.size(); ++i) {
    std::string path = std::string("/assets/original/") + names[i];
    SDL_AudioSpec source{};
    Uint8* bytes = nullptr;
    Uint32 length = 0;
    if (!SDL_LoadWAV(path.c_str(), &source, &bytes, &length)) return false;
    SDL_AudioCVT cvt{};
    if (SDL_BuildAudioCVT(&cvt, source.format, source.channels, source.freq,
                          audioSpec_.format, audioSpec_.channels, audioSpec_.freq) < 0) {
      SDL_FreeWAV(bytes);
      return false;
    }
    cvt.len = static_cast<int>(length);
    cvt.buf = static_cast<Uint8*>(SDL_malloc(static_cast<size_t>(length) * cvt.len_mult));
    std::memcpy(cvt.buf, bytes, length);
    SDL_FreeWAV(bytes);
    if (SDL_ConvertAudio(&cvt) < 0) {
      SDL_free(cvt.buf);
      return false;
    }
    const auto* samples = reinterpret_cast<const float*>(cvt.buf);
    clips_[i].samples.assign(samples, samples + cvt.len_cvt / sizeof(float));
    SDL_free(cvt.buf);
  }
  SDL_PauseAudioDevice(audioDevice_, 0);
  return true;
}

void Gates32::playSound(int id) {
  if (!soundEnabled_ || id < 0 || id >= static_cast<int>(clips_.size()) || !audioDevice_) return;
  SDL_LockAudioDevice(audioDevice_);
  Voice* selected = nullptr;
  for (Voice& voice : voices_) {
    if (!voice.clip) { selected = &voice; break; }
  }
  if (!selected) selected = &voices_[0];
  selected->clip = &clips_[id];
  selected->position = 0;
  SDL_UnlockAudioDevice(audioDevice_);
}

bool Gates32::initialize() {
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_EVENTS) != 0) return false;
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
  window_ = SDL_CreateWindow("Gates32 r.m10 - Web reconstruction",
                             SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                             kWidth * 2, kHeight * 2,
                             SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
#ifdef __EMSCRIPTEN__
  // The software SDL renderer still presents through the browser canvas and
  // avoids EGL changing main-loop timing before Emscripten owns the loop.
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
#else
  renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED);
#endif
  if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
  if (!window_ || !renderer_) return false;
  SDL_RenderSetLogicalSize(renderer_, kWidth, kHeight);
#ifdef __EMSCRIPTEN__
  // The browser canvas is sized continuously to fit the available viewport.
  // Integer-only renderer scaling would otherwise leave a native-size image
  // floating inside a larger black canvas whenever the fit is not exactly 2x.
  SDL_RenderSetIntegerScale(renderer_, SDL_FALSE);
#else
  SDL_RenderSetIntegerScale(renderer_, SDL_TRUE);
#endif
  titleBackgroundTexture_ = SDL_CreateTexture(
      renderer_, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
      kWidth, kHeight);
  if (titleBackgroundTexture_)
    SDL_SetTextureBlendMode(titleBackgroundTexture_, SDL_BLENDMODE_NONE);
  if (!loadImages() || !loadSpriteInfo() || !loadMathTables()) return false;
  const bool audioReady = loadAudio();
  if (!audioReady) SDL_Log("Audio disabled: %s", SDL_GetError());
#ifdef __EMSCRIPTEN__
  EM_ASM({
    window.gates32Diagnostics = {};
    window.gates32Diagnostics.audioReady = !!$0;
    window.gates32Diagnostics.audioStatus = $1;
    window.gates32Diagnostics.logicalWidth = 384;
    window.gates32Diagnostics.logicalHeight = 400;
    window.gates32Diagnostics.engine = 'SDL2/Emscripten';
    window.gates32Diagnostics.persistenceReady = !!window.gates32PersistenceReady;
    window.gates32Diagnostics.persistenceError = window.gates32PersistenceError || String();
    var status = document.getElementById('status');
    if (status) {
      status.dataset.audioReady = $0 ? 'true' : 'false';
      status.dataset.audioStatus = String($1);
      status.dataset.persistenceReady = window.gates32PersistenceReady ? 'true' : 'false';
      status.dataset.persistenceError = window.gates32PersistenceError || String();
      status.dataset.titleBackgroundTarget = $2 ? 'true' : 'false';
    }
  }, audioReady ? 1 : 0, audioDevice_ ? static_cast<int>(SDL_GetAudioDeviceStatus(audioDevice_)) : 0,
     titleBackgroundTexture_ ? 1 : 0);
#endif
  loadRanking();
  loadBundledReplay();
#ifdef __EMSCRIPTEN__
  if (EM_ASM_INT({ return new URLSearchParams(location.search).get('trace') === '1' ? 1 : 0; })) {
    traceCompact_ = EM_ASM_INT({
      return new URLSearchParams(location.search).get('traceCompact') === '1' ? 1 : 0;
    }) != 0;
    traceEndStates_ = EM_ASM_INT({
      return new URLSearchParams(location.search).get('traceEndStates') === '1' ? 1 : 0;
    }) != 0;
    traceFrameLimit_ = static_cast<uint64_t>(std::max(0, EM_ASM_INT({
      var value = Number(new URLSearchParams(location.search).get('traceFrames'));
      return Number.isFinite(value) ? Math.floor(value) : 0;
    })));
    traceFrameStart_ = static_cast<uint64_t>(std::max(1, EM_ASM_INT({
      var value = Number(new URLSearchParams(location.search).get('traceStart'));
      return Number.isFinite(value) && value > 0 ? Math.floor(value) : 1;
    })));
    replayPlayback_ = !replayInput_.empty();
    replayRecording_ = false;
    replayPosition_ = 0;
    tickMs_ = 1;
    if (replayPlayback_) {
      explosionMode_ = (replayHeader_.modeFlags & 4u) != 0;
      acchohMode_ = (replayHeader_.modeFlags & 8u) != 0;
      soundEnabled_ = (replayHeader_.modeFlags & 0x10u) == 0;
      startGame(replayHeader_.seed, static_cast<int>(replayHeader_.randomIndex));
      // Browser automation tabs may suspend requestAnimationFrame completely.
      // Trace mode therefore runs only the deterministic 40 Hz logic here.
      const bool restoreSound = soundEnabled_;
      soundEnabled_ = false;
      for (size_t guard = 0; guard <= replayInput_.size() && traceActive_; ++guard) update();
      soundEnabled_ = restoreSound;
    }
  }
#endif
  lastBrowserTime_ = SDL_GetTicks();
  return true;
}

void Gates32::shutdown() {
  if (audioDevice_) SDL_CloseAudioDevice(audioDevice_);
  SDL_DestroyTexture(titleBackgroundTexture_);
  SDL_DestroyTexture(titleTexture_);
  SDL_DestroyTexture(backTexture_);
  SDL_DestroyTexture(earthTexture_);
  SDL_DestroyTexture(spriteTexture_);
  SDL_DestroyTexture(font8Texture_);
  SDL_DestroyTexture(font16Texture_);
  SDL_DestroyRenderer(renderer_);
  SDL_DestroyWindow(window_);
  SDL_Quit();
}

Entity* Gates32::allocate(std::vector<Entity>& pool, EntityKind kind) {
  const bool managedEffectPool = &pool == &effects_;
  const bool managedBombPool = &pool == &bombEffects_;
  if (managedEffectPool && effectManagerCount_ >= effects_.size()) return nullptr;
  if (managedBombPool && bombEffectManagerCount_ >= bombEffects_.size()) return nullptr;
  for (Entity& entity : pool) {
    if (!entity.active) {
      const int32_t staleX = entity.x;
      const int32_t staleY = entity.y;
      const int32_t staleVx = entity.vx;
      const int32_t staleVy = entity.vy;
      const int staleAngle = entity.angle;
      const int32_t staleSlotData30 = entity.slotData30;
      const int staleAnimeDelay = entity.animeDelay;
      const int staleFrameFirst = entity.frameFirst;
      const int staleFrameLast = entity.frameLast;
      const uint32_t staleScriptPc = entity.scriptPc;
      const bool staleBounce = entity.bounce;
      entity = {};
      // FUN_004065e0 only writes the type/callback before event 1. Most
      // spawners then replace the integer coordinate words, leaving the old
      // 16-bit fractions (and some type-specific bytes) in a reused slot.
      entity.x = staleX;
      entity.y = staleY;
      entity.angle = staleAngle;
      entity.slotData30 = staleSlotData30;
      // The type-specific event-1 constructors do not clear the common
      // movement mode at +0x18. A slot once used by the hard Gates2 can thus
      // make a later ordinary enemy retain edge-bounce behaviour.
      if (&pool == &enemies_) entity.bounce = staleBounce;
      if (kind == EntityKind::Effect) {
        entity.vx = staleVx;
        entity.vy = staleVy;
        // NAnime event 1 clears +0x20, +0x28 and +0x2c only. Rect-emitter
        // width/height remain readable when its just-retired record aliases
        // the child returned by the allocator.
        entity.frameFirst = staleFrameFirst;
        entity.frameLast = staleFrameLast;
      }
      // Type 0x80 is registered with FUN_00406080. Its event-1 path is a
      // no-op, so a reused projectile keeps an interrupted NAnime program.
      // This is observable when an off-screen hit projectile's slot is
      // allocated again before the old animation state has been overwritten.
      if (kind == EntityKind::EnemyBullet) {
        entity.animeDelay = staleAnimeDelay;
        entity.scriptPc = staleScriptPc;
      }
      entity.active = true;
      entity.kind = kind;
      if (managedEffectPool) ++effectManagerCount_;
      if (managedBombPool) ++bombEffectManagerCount_;
      return &entity;
    }
  }
  if (managedEffectPool) {
    // FUN_004066e0 returns the first record just past the logical pool when
    // its cached active count says there is room but every logical slot is
    // occupied. The native allocation includes two padding records, so the
    // callers safely write to that inactive record and continue (including
    // any subsequent RNG calls), although the manager never updates it.
    effectOverflow_.active = false;
    effectOverflow_.kind = EntityKind::None;
    return &effectOverflow_;
  }
  return nullptr;
}

Entity* Gates32::allocateWorld(std::vector<Entity>& pool, EntityKind kind) {
  // The native game uses separate managers: 32 enemy bodies and
  // worldCapacity_ enemy projectiles. They do not share an active quota.
  if (&pool == &enemies_) {
    if (enemyManagerCount_ >= enemies_.size()) return nullptr;
    Entity* entity = allocate(pool, kind);
    if (entity) ++enemyManagerCount_;
    return entity;
  }
  if (&pool == &enemyBullets_) {
    if (enemyBulletManagerCount_ >= enemyBullets_.size()) return nullptr;
    Entity* entity = allocate(pool, kind);
    if (entity) ++enemyBulletManagerCount_;
    return entity;
  }
  return allocate(pool, kind);
}

int Gates32::randomSigned() {
  // All callers pre-increment DAT_0041adf4, and index 1024 aliases the
  // following zero-valued first sine entry before wrapping on the next call.
  if (++randomIndex_ > 1024) randomIndex_ = 0;
  if (randomIndex_ == 1024) return 0;
  return randomTable_[randomIndex_];
}

int Gates32::aimAngle(int dx, int dy) const {
  // Direct translation of FUN_00401110, including its zero handling and lookup.
  int16_t sx = static_cast<int16_t>(dx == 0 ? 1 : dx);
  int16_t sy = static_cast<int16_t>(dy == 0 ? 1 : dy);
  const int ax = std::abs(static_cast<int>(sx));
  const int ay = std::abs(static_cast<int>(sy));
  int raw = 0;
  if (ay < ax) {
    int q = (sy * 32 + (sy > 0 ? ax / 2 : -ax / 2)) / ax;
    raw = sx < 0 ? 128 - q : q;
  } else if (sy > 0) {
    int q = (sx * 32 + (sx > 0 ? ay / 2 : -ay / 2)) / ay;
    raw = 64 - q;
  } else {
    int q = (sx * 32 + (sx > 0 ? ay / 2 : -ay / 2)) / ay;
    raw = q - 64;
  }
  return static_cast<uint8_t>(atanLut_[static_cast<uint8_t>(raw)]);
}

int32_t Gates32::velocityX16(int angle, double speed) const {
  return static_cast<int32_t>(std::lrint(static_cast<double>(cos16_[angle & 255]) * speed));
}
int32_t Gates32::velocityY16(int angle, double speed) const {
  return static_cast<int32_t>(std::lrint(static_cast<double>(sin16_[angle & 255]) * speed));
}
int32_t Gates32::velocityX32(int angle, double speed) const {
  return static_cast<int32_t>(std::lrint(static_cast<double>(cos32_[angle & 255]) * speed));
}
int32_t Gates32::velocityY32(int angle, double speed) const {
  return static_cast<int32_t>(std::lrint(static_cast<double>(sin32_[angle & 255]) * speed));
}

int16_t Gates32::runtimeI16(uint32_t address) const {
  if (address < kRuntimeBase || address - kRuntimeBase + 2 > runtimeCommon_.size()) return 0;
  int16_t value = 0;
  std::memcpy(&value, runtimeCommon_.data() + (address - kRuntimeBase), sizeof(value));
  return value;
}

int32_t Gates32::runtimeI32(uint32_t address) const {
  if (address < kRuntimeBase || address - kRuntimeBase + 4 > runtimeCommon_.size()) return 0;
  int32_t value = 0;
  std::memcpy(&value, runtimeCommon_.data() + (address - kRuntimeBase), sizeof(value));
  return value;
}

void Gates32::dispatchAnimeEvent(Entity& entity, int event, int) {
  if (entity.kind == EntityKind::Gates2) {
    if (event == 0x10000) {
      entity.mode = 1;
      entity.timer = 0;
      entity.aux = 0;
    } else if (event == 0x10001) {
      entity.mode = 2;
      entity.timer = 0;
      entity.aux = 0;
      entity.cycle = 0;
    }
  }
  // FUN_004056f0: event 1 freezes a generic NAnime object and cancels its script.
  if (event == 1) {
    entity.vx = 0;
    entity.vy = 0;
    entity.scriptPc = 0;
  }
}

bool Gates32::advanceNAnime(Entity& entity, int elapsed) {
  if (!entity.active || entity.scriptPc == 0) return entity.active;
  entity.animeDelay -= elapsed;
  if (entity.animeDelay > 0) return true;

  // Direct translation of FUN_00405740. Zero-duration commands deliberately
  // continue in the same tick; the guard only protects malformed external data.
  for (int guard = 0; guard < 256 && entity.scriptPc != 0; ++guard) {
    const uint32_t record = entity.scriptPc;
    const int16_t duration = runtimeI16(record);
    const int16_t opcode = runtimeI16(record + 2);
    entity.animeDelay += duration;
    entity.scriptPc = record + 4;

    switch (opcode) {
      case 1: {
        const int16_t sprite = runtimeI16(entity.scriptPc);
        entity.scriptPc = record + 6;
        entity.sprite = sprite;
        break;
      }
      case 2: {
        const int event = runtimeI32(entity.scriptPc);
        const int argument = runtimeI32(entity.scriptPc + 4);
        entity.scriptPc = record + 12;
        dispatchAnimeEvent(entity, event, argument);
        if (entity.scriptPc == 0) return entity.active;
        break;
      }
      case 3: {
        const int16_t words = runtimeI16(entity.scriptPc);
        entity.scriptPc = record + 6 + static_cast<int32_t>(words) * 2;
        break;
      }
      case 4: {
        const int16_t scale = runtimeI16(entity.scriptPc);
        entity.scriptPc = record + 6;
        entity.vx = static_cast<int32_t>((static_cast<int64_t>(scale) * entity.vx) >> 8);
        entity.vy = static_cast<int32_t>((static_cast<int64_t>(scale) * entity.vy) >> 8);
        break;
      }
      case 5: {
        const int scale = (randomSigned() >> 10) + runtimeI16(entity.scriptPc);
        entity.scriptPc = record + 6;
        entity.vx = static_cast<int32_t>((static_cast<int64_t>(scale) * entity.vx) >> 8);
        entity.vy = static_cast<int32_t>((static_cast<int64_t>(scale) * entity.vy) >> 8);
        break;
      }
      case 6: {
        const int16_t divisor = runtimeI16(entity.scriptPc);
        entity.scriptPc = record + 6;
        if (divisor != 0) entity.animeDelay += randomSigned() % divisor;
        break;
      }
      case 0x12: {
        const int event = runtimeI16(entity.scriptPc);
        entity.scriptPc = record + 6;
        dispatchAnimeEvent(entity, event, 0);
        if (entity.scriptPc == 0) return entity.active;
        break;
      }
      case 0x13: {
        const int event = 0x10000 + runtimeI16(entity.scriptPc);
        entity.scriptPc = record + 6;
        dispatchAnimeEvent(entity, event, 0);
        if (entity.scriptPc == 0) return entity.active;
        break;
      }
      case -1:
        entity.scriptPc = 0;
        entity.active = false;
        return false;
      case 0x1000:
        entity.scriptPc = 0;
        return entity.active;
      default:
        // The original treats unknown opcodes as no-argument no-ops.
        break;
    }
    if (entity.animeDelay > 0) return entity.active;
  }
  return entity.active;
}

void Gates32::configurePools() {
  const size_t expanded = (explosionMode_ ? 512u : 0u) + (acchohMode_ ? 1024u : 0u);
  worldCapacity_ = 320u + (acchohMode_ ? 1024u : 0u);
  enemies_.assign(32, {});
  enemyBullets_.assign(worldCapacity_, {});
  playerShots_.assign(32, {});
  effects_.assign(256u + expanded, {});
  // DAT_00590040 has 0x50 records. FUN_00405350 overwrites only the first
  // 64 with the player's radial blast; Gates2 muzzle flashes share all 80.
  bombEffects_.assign(80, {});
  particles_.assign(128u + expanded, {});
  enemyManagerCount_ = 0;
  enemyBulletManagerCount_ = 0;
  effectManagerCount_ = 0;
  bombEffectManagerCount_ = 0;
  particleManagerCount_ = 0;
  effectOverflow_ = {};
#ifdef __EMSCRIPTEN__
  EM_ASM({
    if (window.gates32Diagnostics) {
      var capacity = {};
      capacity.world = $0;
      capacity.anime = $1;
      capacity.shots = 32;
      capacity.particles = $2;
      window.gates32Diagnostics.poolCapacity = capacity;
      var status = document.getElementById('status');
      if (status) {
        status.dataset.poolWorld = String($0);
        status.dataset.poolAnime = String($1);
        status.dataset.poolShots = '32';
        status.dataset.poolParticles = String($2);
      }
    }
  }, static_cast<int>(worldCapacity_), static_cast<int>(effects_.size()),
     static_cast<int>(particles_.size()));
#endif
}

void Gates32::resetObjects() {
  for (auto* pool : {&enemies_, &enemyBullets_, &playerShots_, &effects_, &bombEffects_, &particles_})
    for (Entity& entity : *pool) entity = {};
  effectManagerCount_ = 0;
  enemyBulletManagerCount_ = 0;
  bombEffectManagerCount_ = 0;
  particleManagerCount_ = 0;
  effectOverflow_ = {};
}

void Gates32::startGame(uint32_t seed, int randomIndex) {
  replaySession_ = replayPlayback_;
  titleTickAccumulator_ = 0;
  titleIdleCounter_ = 0;
  titlePreviousInput_ = 0;
  replayAbortRequested_ = false;
  configurePools();
  randomSeed_ = seed ? seed : 1;
  uint32_t state = randomSeed_;
  for (int i = 0; i < 1024; ++i) {
    state = state * 214013u + 2531011u; // Microsoft Visual C++ 5 CRT rand().
    randomTable_[i] = static_cast<int16_t>(((state >> 16) & 0x7fff) - 0x8000);
  }
  randomIndex_ = randomIndex;
  playerX_ = 192 * kOne;
  playerY_ = 200 * kOne;
  lives_ = 2;
  invulnerable_ = 0;
  shotCooldown_ = 0;
  piercingShotsEnabled_ = false;
  score_ = 0;
  nextOneUp_ = 100000;
  deflation_ = 0;
  level_ = 1;
  scoreSubmitted_ = false;
  stageState_ = 0;
  stageCounter_ = 0;
  stageCountdown_ = 0;
  stagePeriod_ = 1;
  endTimer_ = 0;
  endStartFrame_ = 0;
  frameNumber_ = 0;
  publishTraceAfterFrame_ = false;
  traceActive_ = replayPlayback_;
  stateTrace_.clear();
  if (traceActive_) {
    stateTrace_ = traceCompact_
        ? "frame,input,random_index,player_x,player_y,lives,score,level,world_objects,anime,shots,particles,enemy_manager_count,stage_state,stage_counter,stage_countdown,stage_period,anime_manager_count,particle_manager_count,bombs_hash,state_hash\n"
        : "frame,input,random_index,player_x,player_y,lives,score,level,world_objects,anime,shots,particles,world_head,anime_head,shots_head,particles_head,world_all,anime_all,shots_all,particles_all,shots_meta,world_slots,bombs_all,enemy_manager_count,stage_state,stage_counter,stage_countdown,stage_period,anime_manager_count,particle_manager_count,bombs_hash,state_hash\n";
  }
  screen_ = Screen::Playing;
  SDL_PauseAudioDevice(audioDevice_, 0);
#ifdef __EMSCRIPTEN__
  EM_ASM({
    if (window.gates32Diagnostics) window.gates32Diagnostics.audioStatus = $0;
    var status = document.getElementById('status');
    if (status) status.dataset.audioStatus = String($0);
  },
         audioDevice_ ? static_cast<int>(SDL_GetAudioDeviceStatus(audioDevice_)) : 0);
#endif
}

void Gates32::startLiveGame() {
  replayPlayback_ = false;
  replayRecording_ = true;
  replayPosition_ = 0;
  replayInput_.clear();
  const uint32_t seed = currentSeed();
  replayHeader_ = {seed, 0, 0,
                   (explosionMode_ ? 4u : 0u) | (acchohMode_ ? 8u : 0u) |
                       (soundEnabled_ ? 0u : 0x10u)};
  startGame(seed);
#ifdef __EMSCRIPTEN__
  EM_ASM({
    var status = document.getElementById('status');
    if (status) status.dataset.replayRecording = 'true';
  });
#endif
}

void Gates32::startBundledReplay() {
  startReplay(true);
}

void Gates32::startPreviousReplay() {
  startReplay(false);
}

void Gates32::startReplay(bool reloadBundled) {
  if (reloadBundled) loadBundledReplay();
  replayPlayback_ = !replayInput_.empty();
  replayRecording_ = false;
  replayPosition_ = 0;
  if (!replayPlayback_) return;

  explosionMode_ = (replayHeader_.modeFlags & 4u) != 0;
  acchohMode_ = (replayHeader_.modeFlags & 8u) != 0;
  soundEnabled_ = (replayHeader_.modeFlags & 0x10u) == 0;
  startGame(replayHeader_.seed, static_cast<int>(replayHeader_.randomIndex));
#ifdef __EMSCRIPTEN__
  EM_ASM({
    if (window.gates32Diagnostics) {
      window.gates32Diagnostics.demoStarted = true;
      window.gates32Diagnostics.screen = 'playing';
    }
    var status = document.getElementById('status');
    if (status) {
      status.dataset.demoStarted = 'true';
      status.dataset.replaySource = $0 ? 'bundled' : 'previous';
      status.dataset.replayFrames = String($1);
      status.dataset.replayRecording = 'false';
    }
  }, reloadBundled ? 1 : 0, static_cast<int>(replayInput_.size()));
#endif
}

void Gates32::finishReplayRecording() {
  if (!replayRecording_) return;
  replayRecording_ = false;
  replayHeader_.length = static_cast<uint32_t>(replayInput_.size());
#ifdef __EMSCRIPTEN__
  EM_ASM({
    var status = document.getElementById('status');
    if (status) {
      status.dataset.replayRecording = 'false';
      status.dataset.lastReplayFrames = String($0);
    }
  }, static_cast<int>(replayInput_.size()));
#endif
}

uint8_t Gates32::readLiveInputMask() const {
  const Uint8* keys = SDL_GetKeyboardState(nullptr);
  uint8_t value = 0;
  if (keys[SDL_SCANCODE_DOWN]) value |= 0x02;
  if (keys[SDL_SCANCODE_LEFT]) value |= 0x04;
  if (keys[SDL_SCANCODE_RIGHT]) value |= 0x08;
  if (keys[SDL_SCANCODE_UP]) value |= 0x10;
  if (keys[SDL_SCANCODE_SPACE] || keys[SDL_SCANCODE_KP_0] || keys[SDL_SCANCODE_Z] ||
      keyFire_ || clickFire_) value |= 0x20;
#ifdef __EMSCRIPTEN__
  value |= static_cast<uint8_t>(EM_ASM_INT({
    return ((window.gates32VirtualInput || 0) |
            (window.gates32GamepadInput || 0)) & 0x3e;
  }));
#endif
  return value;
}

uint8_t Gates32::readInputMask() {
  if (replayPlayback_) {
    // The original replay callback returns to the title as soon as live input
    // is detected, and also does so after consuming the playback sentinel.
    if (readLiveInputMask() != 0) {
      replayAbortRequested_ = true;
      return 0;
    }
    // FUN_00405070 consumes every byte while cursor < recorded count. Once
    // exhausted, replay mode remains active and supplies zero input until the
    // deterministic game-over/clear path returns to the title.
    const size_t playbackFrames = replayInput_.size();
    if (replayPosition_ < playbackFrames) {
      const uint8_t value = replayInput_[replayPosition_++];
      if (traceActive_ && replayPosition_ == playbackFrames) publishTraceAfterFrame_ = true;
      return value;
    }
    return 0;
  }
  const uint8_t value = readLiveInputMask();
  if (replayRecording_) replayInput_.push_back(value);
  return value;
}

void Gates32::handleEvents() {
  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    if (event.type == SDL_QUIT) running_ = false;
    if (event.type == SDL_MOUSEBUTTONDOWN) {
      clickFire_ = true;
    }
    if (event.type == SDL_MOUSEBUTTONUP) clickFire_ = false;
    if (event.type == SDL_KEYDOWN && !event.key.repeat) {
      const SDL_Keycode key = event.key.keysym.sym;
      if (key == SDLK_ESCAPE || key == SDLK_F12) running_ = false;
      if ((key == SDLK_RETURN || key == SDLK_KP_ENTER) && screen_ == Screen::Title) {
        startLiveGame();
      }
      if (key == SDLK_SPACE || key == SDLK_KP_0 || key == SDLK_z) {
        keyFire_ = true;
      }
      if (key == SDLK_F2) {
        startBundledReplay();
      }
      if (key == SDLK_F3) saveReplay();
      if (key == SDLK_F4) {
        replayInput_.clear();
        replayRecording_ = true;
        replayPlayback_ = false;
        replayHeader_ = {currentSeed(), 0, 0,
                         (explosionMode_ ? 4u : 0u) | (acchohMode_ ? 8u : 0u) |
                             (soundEnabled_ ? 0u : 0x10u)};
        startGame(replayHeader_.seed);
      }
      if (key == SDLK_F7) soundEnabled_ = !soundEnabled_;
      if (key == SDLK_F6) saveStateTrace();
      if (key == SDLK_F8) explosionMode_ = !explosionMode_;
      if (key == SDLK_F9) acchohMode_ = !acchohMode_;
      if (key == SDLK_F10) tickMs_ = 1; // deterministic trace fast-forward
      if (key == SDLK_F11) backgroundEnabled_ = !backgroundEnabled_;
      if (key >= SDLK_1 && key <= SDLK_9) tickMs_ = std::max(4, 1000 / ((key - SDLK_0) * 10));
      if (key == SDLK_0) tickMs_ = kTickMs;
    }
    if (event.type == SDL_KEYUP) {
      if (event.key.keysym.sym == SDLK_SPACE || event.key.keysym.sym == SDLK_KP_0 ||
          event.key.keysym.sym == SDLK_z) keyFire_ = false;
    }
  }
#ifdef __EMSCRIPTEN__
  const bool browserStart = EM_ASM_INT({
    var pressed = !!window.gates32VirtualStart || !!window.gates32GamepadStart;
    window.gates32VirtualStart = false;
    window.gates32GamepadStart = false;
    return pressed ? 1 : 0;
  }) != 0;
  if (browserStart && screen_ == Screen::Title) startLiveGame();
#endif
}

void Gates32::updateTitle() {
  // FUN_00403ce0 polls the title at 100 ms intervals. After 160 identical
  // input samples (about 16 seconds at rest), command 4 starts the bundled
  // replay using its saved seed, random-table cursor, and mode flags.
  titleTickAccumulator_ += tickMs_;
  while (titleTickAccumulator_ >= 100 && screen_ == Screen::Title) {
    titleTickAccumulator_ -= 100;
    const uint8_t input = readLiveInputMask();
    if (input == titlePreviousInput_) {
      ++titleIdleCounter_;
    } else {
      titleIdleCounter_ = 0;
    }
    titlePreviousInput_ = input;

    if (titleIdleCounter_ > 0x9f) {
      // Command 4 reuses the input bytes, seed, random cursor, and modes
      // retained from the immediately preceding live play. The bundled file
      // is only the initial value before the first play of this process.
      startPreviousReplay();
      return;
    }
  }
}

void Gates32::spawnPlayerShot() {
  Entity* shot = allocate(playerShots_, EntityKind::PlayerShot);
  if (!shot) return;
  shot->x = playerX_ - 2 * kOne;
  shot->y = playerY_;
  shot->vy = -16 * kOne;
  shot->sprite = 2;
  shot->animeDelay = 0;
  shot->piercing = piercingShotsEnabled_;
  playSound(0);
}

void Gates32::spawnEffect(int x, int y, int first, int last, int delay, int32_t vx, int32_t vy) {
  uint32_t script = 0;
  if (first == 24 && last == 29 && delay == 4) script = kScriptBulletHit;
  else if (first == 31 && last == 39 && delay == 6) script = kScriptBomb;
  else if (first == 42 && last == 47) script = kScriptExplosion;
  else if (first == 72 && last == 75) script = kScriptShotHit;
  else if (first == 64 && last == 64) script = kScriptMuzzle;
  if (script != 0) {
    spawnAnime(x, y, script, vx, vy);
    return;
  }

  Entity* effect = allocate(effects_, EntityKind::Effect);
  if (!effect) return;
  effect->x = withIntegerPart(effect->x, x);
  effect->y = withIntegerPart(effect->y, y);
  effect->vx = vx;
  effect->vy = vy;
  effect->frameFirst = first;
  effect->frameLast = last;
  effect->sprite = first;
  effect->frameDelay = std::max(1, delay);
  effect->timer = effect->frameDelay;
}

Entity* Gates32::spawnAnime(int x, int y, uint32_t script, int32_t vx,
                            int32_t vy, int initialDelay, int sprite) {
  Entity* effect = allocate(effects_, EntityKind::Effect);
  if (!effect) return nullptr;
  effect->x = withIntegerPart(effect->x, x);
  effect->y = withIntegerPart(effect->y, y);
  effect->vx = vx;
  effect->vy = vy;
  effect->sprite = sprite;
  effect->scriptPc = script;
  effect->animeDelay = initialDelay;
  effect->callback = EffectCallback::NAnime;
  effect->collidable = false;
  return effect;
}

void Gates32::spawnParticles(int x, int y, int count) {
  for (int i = 0; i < count; ++i) {
    // FUN_004065a0 rejects an allocation from the manager's cached active
    // count before scanning for a physically free record. FUN_00406500 counts
    // particles before retiring out-of-bounds records, so a slot freed in the
    // current manager pass is intentionally unavailable until the next pass.
    if (particleManagerCount_ >= particles_.size()) return;
    Entity* particle = allocate(particles_, EntityKind::Particle);
    if (!particle) return;
    ++particleManagerCount_;
    particle->x = withIntegerPart(particle->x, x);
    particle->y = withIntegerPart(particle->y, y);
    const int angle = static_cast<uint8_t>(randomSigned());
    // FUN_00405520: each hit creates eight palette-index-15 points with the
    // original 32-bit sine/cosine vector multiplied by four.
    particle->vx = cos32_[angle] * 4;
    particle->vy = sin32_[angle] * 4;
    particle->sprite = -1;
    particle->collidable = false;
  }
}

void Gates32::spawnTrailEmitter(int x, int y, int32_t vx, int32_t vy, int timer) {
  Entity* emitter = allocate(effects_, EntityKind::Effect);
  if (!emitter) return;
  emitter->x = withIntegerPart(emitter->x, x);
  emitter->y = withIntegerPart(emitter->y, y);
  emitter->vx = vx;
  emitter->vy = vy;
  emitter->animeDelay = timer;
  emitter->sprite = -1;
  emitter->callback = EffectCallback::TrailEmitter;
  emitter->collidable = false;
}

void Gates32::spawnExplosionEmitter(int x, int y, int timer, uint32_t script,
                                    int32_t vx, int32_t vy,
                                    bool assignVelocity) {
  Entity* emitter = allocate(effects_, EntityKind::Effect);
  if (!emitter) return;
  emitter->x = withIntegerPart(emitter->x, x);
  emitter->y = withIntegerPart(emitter->y, y);
  if (assignVelocity) {
    emitter->vx = vx;
    emitter->vy = vy;
  }
  emitter->animeDelay = timer;
  emitter->scriptPc = script;
  emitter->sprite = -1;
  emitter->callback = EffectCallback::ExplosionEmitter;
  emitter->collidable = false;
}

void Gates32::spawnRectEmitter(int x, int y, int width, int height, int timer) {
  Entity* emitter = allocate(effects_, EntityKind::Effect);
  if (!emitter) return;
  emitter->x = withIntegerPart(emitter->x, x);
  emitter->y = withIntegerPart(emitter->y, y);
  emitter->animeDelay = timer;
  // 00406f48/00406f52 write the rectangle dimensions to words +0x12/+0x16,
  // the high halves of the velocity dwords. Their reused low halves survive.
  emitter->vx = withIntegerPart(emitter->vx, width);
  emitter->vy = withIntegerPart(emitter->vy, height);
  emitter->frameFirst = width;
  emitter->frameLast = height;
  emitter->sprite = -1;
  emitter->callback = EffectCallback::RectEmitter;
  emitter->collidable = false;
}

void Gates32::spawnRadialEmitter(int x, int y, int speedX, int speedY, int count,
                                 uint32_t script) {
  Entity* emitter = allocate(effects_, EntityKind::Effect);
  if (!emitter) return;
  emitter->x = withIntegerPart(emitter->x, x);
  emitter->y = withIntegerPart(emitter->y, y);
  emitter->savedVx = speedX;
  emitter->savedVy = speedY;
  emitter->aux = count;
  emitter->animeDelay = speedX;
  emitter->scriptPc = script;
  emitter->sprite = -1;
  emitter->callback = EffectCallback::RadialEmitter;
  emitter->collidable = false;
}

void Gates32::spawnBomb() {
  const int x = fixedToInt(playerX_);
  const int y = fixedToInt(playerY_);
  size_t index = 0;
  for (int rawAngle = 0x4000; rawAngle > 0; rawAngle -= 0x100, ++index) {
    Entity& effect = bombEffects_[index];
    const int32_t staleX = effect.x;
    const int32_t staleY = effect.y;
    const int staleSprite = effect.sprite;
    effect = {};
    effect.active = true;
    effect.kind = EntityKind::Effect;
    effect.x = withIntegerPart(staleX, x);
    effect.y = withIntegerPart(staleY, y);
    const int tableIndex = rawAngle / 64;
    effect.vx = runtimeI16(0x004195f0u + static_cast<uint32_t>(tableIndex * 2)) * 20;
    effect.vy = runtimeI16(0x00419670u + static_cast<uint32_t>(tableIndex * 2)) * 20;
    // FUN_00405350 leaves offset 0x20 untouched. Fresh records therefore
    // skip collision on their same-frame first callback; the script then
    // assigns sprite 31 before movement. Reused records retain their pointer.
    effect.sprite = staleSprite;
    effect.scriptPc = kScriptBomb;
    effect.callback = EffectCallback::NAnime;
    // Marks LAB_004053e0, whose update performs the damaging rectangle
    // queries. Ordinary NAnime muzzle flashes in this same manager use
    // FUN_004056f0 and must only animate/move.
    effect.collidable = true;
  }
  playSound(9);
}

void Gates32::spawnBombMuzzle(int x, int y) {
  Entity* effect = allocate(bombEffects_, EntityKind::Effect);
  if (!effect) return;
  // FUN_00406740 writes only the integer origin and script pointer after the
  // NAnime event-1 constructor. The shared slot's fixed-point fractions and
  // velocity therefore survive a previous player explosion.
  effect->x = withIntegerPart(effect->x, x);
  effect->y = withIntegerPart(effect->y, y);
  effect->scriptPc = kScriptMuzzle;
  effect->callback = EffectCallback::NAnime;
  effect->collidable = false;
}

void Gates32::updatePlayer(uint8_t input) {
  // FUN_00405070 wraps the entire player callback in 0 <= stock < 24.
  // Stock zero is therefore still a live, controllable state; only -1 is the
  // true game-over state. The 1UP test runs before movement and collision and
  // awards at most one stock per frame.
  if (lives_ < 0 || lives_ >= 24) return;
  if (score_ >= nextOneUp_) {
    ++lives_;
    deflation_ += nextOneUp_ / 100000; // magic divide at 004051aa
    nextOneUp_ += 100000;
  }

  int dx = 0, dy = 0;
  if (input & 0x04) --dx;
  if (input & 0x08) ++dx;
  if (input & 0x10) --dy;
  if (input & 0x02) ++dy;
  const int speed = (dx && dy) ? 2 : 3;
  playerX_ += dx * speed * kOne;
  playerY_ += dy * speed * kOne;

  if (input & 0x20) {
    // FUN_00405680 advances the -1 latch only while fire is held. Releasing
    // the button leaves it at -1, so a later press arms on its first frame
    // and creates the next shot on the following frame.
    if (shotCooldown_ < 0) {
      ++shotCooldown_;
    } else {
      spawnPlayerShot();
      shotCooldown_ = -1;
    }
  }

  // FUN_00405070 fires before clamping the player's integer coordinates.
  // A boundary-crossing shot therefore inherits the transient out-of-range
  // position (for example x=-2 produces a shot at x=-4) even though the
  // player is clamped back to the edge later in the same callback.
  playerX_ = std::clamp(playerX_, 0, 383 * kOne);
  playerY_ = std::clamp(playerY_, 0, 391 * kOne);

  if (invulnerable_ > 0) --invulnerable_;
  if (invulnerable_ <= 0) {
    for (Entity& bullet : enemyBullets_) {
      if (!bullet.active || !bullet.collidable || !intersectsPlayer(bullet)) continue;
      --lives_;
      // The original decrements the 16-bit stock before the projectile's
      // event-0x100 callback awards its score.
      hitEnemyBullet(bullet);
      if (lives_ == 0) {
        piercingShotsEnabled_ = true;
        for (Entity& shot : playerShots_) if (shot.active) shot.piercing = true;
      }
      invulnerable_ = 30;
      spawnBomb();
      playSound(5);
      break;
    }
  }

}

Entity* Gates32::spawnEnemy(EntityKind kind, int x, int y, int angle, double speed) {
  Entity* enemy = allocateWorld(enemies_, kind);
  if (!enemy) return nullptr;
  const int allocationX = fixedToInt(enemy->x);
  const int allocationY = fixedToInt(enemy->y);
  enemy->x = withIntegerPart(enemy->x, x);
  enemy->y = withIntegerPart(enemy->y, y);
  enemy->vx = velocityX16(angle, speed);
  enemy->vy = velocityY16(angle, speed);
  enemy->angle = angle;
  switch (kind) {
    case EntityKind::Enemy1:
      // FUN_004065e0 sends event 1 before FUN_00407950 assigns the random
      // edge position. Enemy1 therefore captures its firing angle while the
      // freshly cleared object is still at (0, 0); its travel angle is set
      // independently by FUN_00407950.
      enemy->angle = aimAngle(fixedToInt(playerX_) - allocationX + 26,
                              fixedToInt(playerY_) - allocationY + 35);
      enemy->sprite = 770; enemy->hp = enemy->initialHp = 0; enemy->timer = 16;
      enemy->slotData30 = (enemy->slotData30 & ~0xff) |
                          static_cast<uint8_t>(enemy->angle);
      break;
    case EntityKind::Enemy2:
      // Enemy2's event-1 handler leaves byte +0x30 untouched. The enemy pool
      // is shared with Gates objects, so a killed Gates record contributes an
      // HP low byte of 0xff when this physical slot is reused.
      enemy->angle = static_cast<uint8_t>(enemy->slotData30);
      enemy->sprite = 771; enemy->hp = enemy->initialHp = 0; enemy->timer = 16;
      break;
    case EntityKind::Gates1:
      enemy->sprite = 769; enemy->hp = enemy->initialHp = 10; enemy->timer = 16;
      enemy->slotData30 = (1 << 16) | static_cast<uint16_t>(enemy->hp);
      // The event-1 constructor briefly assigns unit speed, but FUN_00407950
      // then overwrites it with the caller's travel speed after positioning.
      break;
    case EntityKind::Gates2:
      enemy->sprite = 769; enemy->hp = enemy->initialHp = 20; enemy->timer = 0;
      // FUN_004070c0 seeds +0x3a with the initial player-facing angle. Mode 0
      // later uses its low nibble as the 16-volley long-pause counter before
      // incrementing it; it is not a zero-based shot count.
      enemy->phase = angle;
      enemy->slotData30 = (1 << 16) | static_cast<uint16_t>(enemy->hp);
      break;
    default: break;
  }
  return enemy;
}

Entity* Gates32::spawnEnemyBullet(int x, int y, int angle, double speed,
                                  int spriteBase, EntityKind kind) {
  Entity* bullet = allocateWorld(enemyBullets_, kind);
  if (!bullet) return nullptr;
  bullet->x = withIntegerPart(bullet->x, x);
  bullet->y = withIntegerPart(bullet->y, y);
  bullet->angle = clampAngle(angle);
  bullet->vx = velocityX32(angle, speed);
  bullet->vy = velocityY32(angle, speed);
  bullet->sprite = spriteBase + ((clampAngle(angle) >> 3) & 31);
  bullet->collidable = true;
  return bullet;
}

void Gates32::updateStage() {
  stageCountdown_ -= stagePeriod_;
  stageCounter_ += level_;
  if (stageCountdown_ > 0) return;

  auto randomEdge = [&](EntityKind kind, double speed, bool forceTop = false) -> Entity* {
    // FUN_00407a50 asks the 32-slot enemy manager for a record before it
    // chooses an edge. A full manager therefore consumes no RNG at all.
    if (enemyManagerCount_ >= enemies_.size()) return nullptr;
    bool hasFreeEnemySlot = false;
    for (const Entity& enemy : enemies_) {
      if (!enemy.active) {
        hasFreeEnemySlot = true;
        break;
      }
    }
    if (!hasFreeEnemySlot) return nullptr;
    int x = 0, y = 0;
    // FUN_00407a50 advances the RNG for the edge selector even when the
    // caller's mask forces the result to the top edge.
    const uint16_t randomSelector = static_cast<uint16_t>(randomSigned());
    const uint16_t selector = forceTop ? 3 : randomSelector;
    if ((selector & 1) == 0) {
      y = static_cast<uint16_t>(randomSigned()) % 400;
      x = (selector & 2) ? -6 : 383;
    } else {
      y = -6;
      x = static_cast<uint16_t>(randomSigned()) % 384;
    }
    const int angle = aimAngle(fixedToInt(playerX_) - x + 26,
                               fixedToInt(playerY_) - y + 35);
    return spawnEnemy(kind, x, y, angle, speed);
  };

  switch (stageState_) {
    case 0:
      stageCountdown_ = 8;
      randomEdge(EntityKind::Enemy1, 2.0);
      if (stageCounter_ > 300) { stageState_ = 1; stageCounter_ = 0; }
      break;
    case 1:
      if (stageCounter_ > 90) { stageState_ = 2; stageCounter_ = 0; }
      break;
    case 2:
      stageCountdown_ = 32;
      randomEdge(EntityKind::Gates1, 2.0);
      if (stageCounter_ > 450) { stageState_ = 3; stageCounter_ = 0; }
      break;
    case 3:
      if (stageCounter_ > 180) { stageState_ = 4; stageCounter_ = 0; }
      break;
    case 4: {
      stageCountdown_ = 64;
      Entity* enemy = randomEdge(EntityKind::Gates2, 2.0);
      if (enemy) { enemy->mode = 2; enemy->modeLocked = true; }
      if (stageCounter_ > 300) { stageState_ = 5; stageCounter_ = 0; }
      break;
    }
    case 5:
      if (stageCounter_ > 180) { stageState_ = 6; stageCounter_ = 0; }
      break;
    case 6:
      stageCountdown_ = 8;
      randomEdge(EntityKind::Enemy2, 2.0);
      if (stageCounter_ > 360) { stageState_ = 7; stageCounter_ = 0; }
      break;
    case 7:
      if (stageCounter_ > 180) { stageState_ = 8; stageCounter_ = 0; }
      break;
    case 8: {
      stageCountdown_ = 64;
      Entity* enemy = randomEdge(EntityKind::Gates2, 1.0, true);
      if (enemy) { enemy->mode = 1; enemy->modeLocked = true; }
      if (stageCounter_ > 450) { stageState_ = 9; stageCounter_ = 0; }
      break;
    }
    case 9:
      if (stageCounter_ > 180) { stageState_ = 10; stageCounter_ = 0; }
      break;
    case 10:
      if (level_ > 3) {
        Entity* enemy = randomEdge(EntityKind::Gates2, 2.0);
        if (enemy) {
          enemy->hard = true;
          enemy->bounce = true;
          // Stage case 10 dispatches event 0x10101 with (level - 4) * 0x100.
          // In particular, level 4 deliberately starts at zero HP and dies on
          // the first effective hit; the original does not impose a minimum.
          enemy->hp = enemy->initialHp = (level_ - 4) * 256;
        }
      }
      stageState_ = 11;
      stageCounter_ = 0;
      break;
    case 11:
      if (stageCounter_ > 300 || level_ < 3) { stageState_ = 12; stageCounter_ = 0; }
      break;
    default:
      // FUN_00407a90 carries the level counter into the next cycle. Only the
      // period, level and state are changed by this branch.
      stagePeriod_ *= 2;
      ++level_;
      stageState_ = 0;
      break;
  }
}

void Gates32::addScore(int base) {
  int shift;
  const int lost = 2 - lives_;
  if (lost < 1) shift = level_ - (deflation_ >> 4) - 1;
  else shift = lost * 2 - (deflation_ >> 4) - 1 + level_;
  shift = std::min(shift, 12);
  const int64_t amount = shift < 1 ? base : static_cast<int64_t>(base) << shift;
  score_ = static_cast<int>(std::min<int64_t>(99999999, static_cast<int64_t>(score_) + amount));
  highScore_ = std::max(highScore_, score_);
}

void Gates32::loadRanking() {
  ranking_ = {};
  auto readTable = [&](const char* path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    input.read(reinterpret_cast<char*>(ranking_.data()),
               static_cast<std::streamsize>(sizeof(ranking_)));
    return input.gcount() == static_cast<std::streamsize>(sizeof(ranking_));
  };
#ifdef __EMSCRIPTEN__
  if (!readTable("/persistent/gates32.sco"))
    readTable("/assets/original/gates32.sco");
#else
  if (!readTable("gates32.sco")) readTable("original/gates32.sco");
#endif
  highScore_ = std::max(0, ranking_[0].score);
#ifdef __EMSCRIPTEN__
  EM_ASM({
    if (window.gates32Diagnostics) {
      window.gates32Diagnostics.rankingLoaded = true;
      window.gates32Diagnostics.highScore = $0;
    }
    var status = document.getElementById('status');
    if (status) {
      status.dataset.rankingLoaded = 'true';
      status.dataset.highScore = String($0);
    }
  }, highScore_);
#endif
}

void Gates32::saveRanking() {
#ifdef __EMSCRIPTEN__
  const char* path = "/persistent/gates32.sco";
#else
  const char* path = "gates32.sco";
#endif
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) return;
  output.write(reinterpret_cast<const char*>(ranking_.data()),
               static_cast<std::streamsize>(sizeof(ranking_)));
  output.close();
#ifdef __EMSCRIPTEN__
  EM_ASM({
    if (window.gates32Diagnostics) window.gates32Diagnostics.rankingSyncPending = true;
    FS.syncfs(false, function(error) {
      if (window.gates32Diagnostics) {
        window.gates32Diagnostics.rankingSyncPending = false;
        window.gates32Diagnostics.rankingSaved = !error;
        window.gates32Diagnostics.rankingSyncError = error ? String(error) : String();
      }
    });
  });
#endif
}

void Gates32::submitScore(bool cleared) {
  if (scoreSubmitted_) return;
  scoreSubmitted_ = true;
  size_t position = 0;
  while (position < ranking_.size() && ranking_[position].score >= score_) ++position;
  if (position == ranking_.size()) return;
  for (size_t i = ranking_.size() - 1; i > position; --i) ranking_[i] = ranking_[i - 1];
  ScoreEntry& entry = ranking_[position];
  entry = {};
  entry.score = score_;
  entry.time = static_cast<int32_t>(std::min<uint64_t>(endStartFrame_, 0x7fffffffu));
  entry.level = level_;
  entry.flags = cleared ? 1 : 0;
  highScore_ = std::max(highScore_, score_);
  saveRanking();
}

void Gates32::hitEnemyBullet(Entity& bullet) {
  if (!bullet.collidable) return;
  addScore(1);
  // callback at 00409190 changes the projectile itself into NAnime instead of
  // allocating a second object; negating the old type makes it non-collidable.
  // It deliberately leaves +0x28 untouched, so a second hit while an old
  // animation is still pending starts the new script with the remaining delay.
  bullet.collidable = false;
  bullet.scriptPc = kScriptBulletHit;
}

void Gates32::hitEnemy(Entity& enemy, const Entity* impact, int damage) {
  if (enemy.kind == EntityKind::Gates1 || enemy.kind == EntityKind::Gates2) addScore(1);
  enemy.hp -= damage;
  // Only Gates types store HP at +0x30. Enemy1/Enemy2 are one-hit objects;
  // their event-0x100 handlers clear the type without touching the angle byte.
  if (enemy.kind == EntityKind::Gates1 || enemy.kind == EntityKind::Gates2) {
    enemy.slotData30 = (enemy.slotData30 & ~0xffff) |
                       static_cast<uint16_t>(enemy.hp);
  }
  // FUN_00406e90/FUN_00407720 emit a moving spark on every fourth remaining
  // hit point for both Gates variants. It copies the integer impact position
  // and the body's velocity into a newly allocated type-0x90 NAnime record.
  if ((enemy.kind == EntityKind::Gates1 || enemy.kind == EntityKind::Gates2) &&
      impact && (enemy.hp & 3) == 0) {
    // Callback 00409280 is an emitter: its own velocity is trace-visible but
    // it stays in place and creates one randomized NAnime child per tick.
    spawnExplosionEmitter(fixedToInt(impact->x), fixedToInt(impact->y), 6,
                          kScriptSpark, enemy.vx, enemy.vy, true);
  }
  if (enemy.kind == EntityKind::Gates2 && !enemy.modeLocked && enemy.hp >= 0) {
    if (enemy.mode == 0 && enemy.hp < enemy.initialHp / 2) {
      enemy.mode = -1;
      enemy.scriptPc = kScriptGatesMode1;
      enemy.animeDelay = 0;
    } else if (enemy.mode == 1 && enemy.hp < enemy.initialHp / 4) {
      enemy.mode = -2;
      enemy.scriptPc = kScriptGatesMode2;
      enemy.animeDelay = 0;
    }
  }
  if (enemy.hp < 0) killEnemy(enemy);
}

void Gates32::killEnemy(Entity& enemy) {
  const int x = fixedToInt(enemy.x), y = fixedToInt(enemy.y);
  switch (enemy.kind) {
    case EntityKind::Enemy1: addScore(16); break;
    case EntityKind::Enemy2: addScore(40); break;
    case EntityKind::Gates1: addScore(128); break;
    case EntityKind::Gates2: addScore(enemy.hard ? 1024 : 256); break;
    default: break;
  }
  if (enemy.kind == EntityKind::Gates1 || enemy.kind == EntityKind::Gates2) {
    // Boss callbacks finish with a 64x64 rectangular chain emitter and a
    // 64-way radial spark emitter (FUN_00406e90/FUN_00407720).
    spawnRectEmitter(x - 6, y + 3, 64, 64, 8);
    spawnRadialEmitter(x + 26, y + 35, 16, 16, 64, kScriptSpark);
    // The boss stays in its world slot while 004056f0 runs the short blinking
    // death script. Its integer origin is shifted along with the rectangle
    // emitter and its existing velocity continues to move it.
    enemy.x = withIntegerPart(enemy.x, x - 6);
    enemy.y = withIntegerPart(enemy.y, y + 3);
    enemy.scriptPc = kScriptGatesDeath;
    enemy.animeDelay = 0;
  } else {
    // FUN_004069d0/FUN_00406b90: 32 ticks, one trail child per four ticks,
    // using the destroyed object's velocity multiplied by six.
    spawnTrailEmitter(x, y, enemy.vx * 6, enemy.vy * 6, 32);
  }
  playSound(enemy.kind == EntityKind::Gates1 || enemy.kind == EntityKind::Gates2 ? 4 : 3);
  if (enemy.kind != EntityKind::Gates1 && enemy.kind != EntityKind::Gates2)
    enemy.active = false;
}

void Gates32::updateEnemies() {
  size_t encountered = 0;
  const int px = fixedToInt(playerX_), py = fixedToInt(playerY_);
  auto clampHardEnemy = [&](Entity& enemy) {
    if (!enemy.bounce || enemy.sprite < 0 ||
        enemy.sprite >= static_cast<int>(sprites_.size())) return;
    const SpriteInfo& sprite = sprites_[enemy.sprite];
    const int x = fixedToInt(enemy.x);
    if (x < 0) {
      enemy.x = withIntegerPart(enemy.x, 0);
      enemy.vx = -enemy.vx;
    } else if (x + sprite.collisionRight >= kWidth) {
      enemy.x = withIntegerPart(enemy.x, kWidth - 1 - sprite.collisionRight);
      enemy.vx = -enemy.vx;
    }
    const int y = fixedToInt(enemy.y);
    if (y < 0) {
      enemy.y = withIntegerPart(enemy.y, 0);
      enemy.vy = -enemy.vy;
    } else if (y + sprite.collisionBottom >= kHeight) {
      enemy.y = withIntegerPart(enemy.y, kHeight - 1 - sprite.collisionBottom);
      enemy.vy = -enemy.vy;
    }
  };
  for (Entity& enemy : enemies_) {
    if (!enemy.active) continue;
    // FUN_004065a0 counts records before invoking their callbacks. An enemy
    // cleared by movement or a collision later in this frame therefore keeps
    // the manager full until the next pass.
    ++encountered;
    if (enemy.scriptPc != 0) {
      const bool deathAnimation = enemy.hp < 0;
      if (!advanceNAnime(enemy)) {
        // FUN_004056f0 still invokes the common mover after opcode -1 has
        // cleared the type. The invisible final coordinate fraction is later
        // inherited when this world slot is allocated again.
        if (deathAnimation) {
          enemy.x += enemy.vx;
          enemy.y += enemy.vy;
          clampHardEnemy(enemy);
        }
        continue;
      }
    }
    if (enemy.hp < 0) {
      enemy.x += enemy.vx;
      enemy.y += enemy.vy;
      // The death animation has switched to the common callback, but +0x18
      // is still intact, so a reused bounce-mode slot remains clamped while
      // its short explosion script is running.
      clampHardEnemy(enemy);
      if (!enemy.bounce && outsideSpriteBounds(enemy)) enemy.active = false;
      continue;
    }
    // FUN_00406c10 sends Gates1 through FUN_004056f0 before its event-0
    // handler. The common callback therefore moves the body before aiming and
    // choosing the two muzzle positions. This becomes observable whenever the
    // fixed-point position crosses an integer boundary on a firing frame.
    const bool movedBeforeUpdate =
        enemy.kind == EntityKind::Gates1 || enemy.kind == EntityKind::Gates2;
    if (movedBeforeUpdate) {
      enemy.x += enemy.vx;
      enemy.y += enemy.vy;
      // FUN_004056f0 reaches FUN_00406140 before the Gates callback. In
      // bounce mode it clamps using the sprite's collision extents and
      // reflects the old velocity before aiming or firing takes place.
      clampHardEnemy(enemy);
    }
    const int x = fixedToInt(enemy.x), y = fixedToInt(enemy.y);
    if (enemy.hard) {
      const int target = aimAngle(px - x - 20, py - y - 40);
      int delta = ((target - enemy.steerAngle + 128) & 255) - 128;
      // 004074d1 uses the same unsigned CMP/SBB idiom as Enemy2. There is no
      // stationary equality case: exact alignment selects the +2 branch.
      enemy.steerAngle = clampAngle(enemy.steerAngle + (delta >= 0 ? 2 : -2));
      enemy.vx = velocityX16(enemy.steerAngle, 3.0);
      enemy.vy = velocityY16(enemy.steerAngle, 3.0);
    }

    if ((enemy.kind != EntityKind::Gates2 || enemy.mode == 0) && --enemy.timer < 0) {
      if (enemy.kind == EntityKind::Enemy1) {
        spawnEnemyBullet(x + 2, y + 20, enemy.angle, 1.0);
        enemy.angle = clampAngle(enemy.angle + 8);
        enemy.slotData30 = (enemy.slotData30 & ~0xff) |
                           static_cast<uint8_t>(enemy.angle);
        enemy.timer = 8;
        playSound(7);
      } else if (enemy.kind == EntityKind::Enemy2) {
        const int target = aimAngle(px - x - 7, py - y - 22);
        spawnEnemyBullet(x + 7, y + 22, target, 1.0);
        enemy.timer = 5;
        playSound(7);
      } else if (enemy.kind == EntityKind::Gates1) {
        const int a = aimAngle(px - x - 26, py - y - 35);
        // FUN_00406d40 allocates these from the world manager as type 0x90.
        // Unlike the ordinary type-0x80 bullet constructor, that clears stale
        // NAnime state while still retaining the reused slot's x/y fraction.
        spawnEnemyBullet(x + 17, y + 31, a, 2.0, 192,
                         EntityKind::EnemyBullet90);
        spawnEnemyBullet(x + 35, y + 29, a, 2.0, 192,
                         EntityKind::EnemyBullet90);
        enemy.timer = ((enemy.phase & 15) == 0) ? 60 : 8;
        ++enemy.phase;
        playSound(7);
      } else if (enemy.kind == EntityKind::Gates2) {
        const int a = aimAngle(px - x - 26, py - y - 35);
        if (enemy.mode == 0) {
          const double speed = enemy.hard ? 5.0 : 2.0;
          if (spawnEnemyBullet(x + 17, y + 31, a, speed, 192,
                               EntityKind::EnemyBullet90))
            spawnBombMuzzle(x + 9, y + 23);
          if (spawnEnemyBullet(x + 35, y + 29, a, speed, 192,
                               EntityKind::EnemyBullet90))
            spawnBombMuzzle(x + 27, y + 21);
          enemy.timer = enemy.hard ? 3 : 8;
          if ((enemy.phase & 15) == 0) enemy.timer = 60;
          ++enemy.phase;
          playSound(7);
        }
      }
    }

    // Gates2 modes 1 and 2 have their own counters and do not use the simple
    // fire branch above (FUN_00407230).
    if (enemy.kind == EntityKind::Gates2 && enemy.mode == 1) {
      ++enemy.aux;
      if (enemy.aux >= 0 && --enemy.timer < 0) {
        const int a = aimAngle(px - x - 27, py - y - 38);
        // The non-hard mode waits until the player is at least 64 pixels away
        // on both axes and within the forward angular quadrant. A failed test
        // leaves the negative timer intact and retries on the next frame.
        const bool canFire = enemy.hard ||
            (static_cast<uint8_t>(a) <= 0x50 &&
             std::abs(py - y - 38) >= 64 &&
             std::abs(px - x - 27) >= 64);
        if (canFire) {
          // FUN_004075c0 allocates all seven records as type 0x90. This
          // clears stale NAnime state that an ordinary type-0x80 projectile
          // would inherit from its reused world slot.
          for (int spread : {32, 16, 8, 0, -8, -16, -32}) {
            if (spawnEnemyBullet(x + 27, y + 38, a + spread, 6.0, 192,
                                 EntityKind::EnemyBullet90))
              spawnBombMuzzle(x + 19, y + 30);
          }
          enemy.timer = 4;
          playSound(8);
          if (enemy.aux > 48) enemy.aux = -24;
        }
      }
    } else if (enemy.kind == EntityKind::Gates2 && enemy.mode == 2) {
      if (--enemy.timer < 1) {
        bool fire = true;
        if (--enemy.aux < 0) {
          if (++enemy.cycle > 1) {
            enemy.cycle = 0;
            enemy.vx = enemy.savedVx;
            enemy.vy = enemy.savedVy;
            enemy.timer = enemy.hard ? 8 : 90;
            fire = false;
          } else {
            if (enemy.vx != 0 || enemy.vy != 0) {
              enemy.savedVx = enemy.vx;
              enemy.savedVy = enemy.vy;
              enemy.vx = enemy.vy = 0;
            }
            enemy.angle = aimAngle(px - x - 26, py - y - 45);
            enemy.aux = 20;
            playSound(6);
          }
        }
        if (fire) {
          for (int spread : {0, 16, -16}) {
            // FUN_00407410 uses the 64-direction projectile sheet. Both the
            // velocity lookup and sprite select discard the low two angle
            // bits, and the records are allocated as type 0x90.
            const int bulletAngle = clampAngle(enemy.angle + spread) & 0xfc;
            Entity* bullet = spawnEnemyBullet(x + 28, y + 45, bulletAngle,
                                              5.2, 225,
                                              EntityKind::EnemyBullet90);
            if (bullet) {
              // VC5's __ftol truncates these x87 products toward zero. The
              // usual table helpers use lrint, which differs by one for some
              // directions (for example angle 92 in this replay).
              bullet->vx = static_cast<int32_t>(
                  static_cast<double>(cos32_[bulletAngle]) * 5.2);
              bullet->vy = static_cast<int32_t>(
                  static_cast<double>(sin32_[bulletAngle]) * 5.2);
              bullet->sprite = 225 + ((bulletAngle >> 2) & 31);
            }
          }
          enemy.timer = 2;
        }
      }
    }

    if (enemy.kind == EntityKind::Enemy2) {
      const int target = aimAngle(px - x - 7, py - y - 22);
      int delta = ((target - enemy.angle + 128) & 255) - 128;
      // 00406c49 uses CMP/SBB to select -1 or +1. Equality does not have a
      // zero case: an exactly aligned Enemy2 advances one angular step.
      enemy.angle = clampAngle(enemy.angle + (delta >= 0 ? 1 : -1));
      enemy.slotData30 = (enemy.slotData30 & ~0xff) |
                         static_cast<uint8_t>(enemy.angle);
      enemy.vx = velocityX16(enemy.angle, 3.0);
      enemy.vy = velocityY16(enemy.angle, 3.0);
    }

    // Enemy1/Enemy2 call FUN_00406140 from their type-specific update and
    // then call it again through FUN_004056f0 -> FUN_00406080. Gates objects
    // only take the common-path step below.
    if (enemy.kind == EntityKind::Enemy1 || enemy.kind == EntityKind::Enemy2) {
      enemy.x += enemy.vx;
      enemy.y += enemy.vy;
      clampHardEnemy(enemy);
      // The type-specific callback calls FUN_00406140 itself.  Its culling
      // therefore happens before FUN_004056f0 performs the common second
      // step.  The callback still advances the now-inactive record once more,
      // which matters when its stale coordinate fraction is reused later.
      if (!enemy.bounce && outsideSpriteBounds(enemy)) {
        enemy.active = false;
      }
    }

    if (!movedBeforeUpdate) {
      enemy.x += enemy.vx;
      enemy.y += enemy.vy;
      clampHardEnemy(enemy);
    }
    if (!enemy.bounce && outsideSpriteBounds(enemy)) {
      enemy.active = false;
    }
  }
  enemyManagerCount_ = encountered;
}

void Gates32::updateEnemyBullets() {
  size_t encountered = 0;
  for (Entity& bullet : enemyBullets_) {
    if (!bullet.active) continue;
    // FUN_004064a0 counts a record before invoking its callback. A bullet
    // culled or destroyed later in this root pass therefore continues to
    // occupy the manager's cached 320-record quota until the next pass.
    ++encountered;
    const bool scripted = bullet.scriptPc != 0;
    const bool alive = !scripted || advanceNAnime(bullet);
    bullet.x += bullet.vx;
    bullet.y += bullet.vy;
    if (!alive) continue;
    if (outsideSpriteBounds(bullet)) bullet.active = false;
  }
  enemyBulletManagerCount_ = encountered;
}

void Gates32::updatePlayerShots() {
  for (Entity& shot : playerShots_) {
    if (!shot.active) continue;
    if (!shot.collidable) {
      const bool alive = advanceNAnime(shot);
      shot.x += shot.vx;
      shot.y += shot.vy;
      if (!alive) continue;
      if (outsideSpriteBounds(shot)) shot.active = false;
      continue;
    }

    // LAB_00405620 takes an NAnime/common step before the first collision
    // query while animeDelay is non-negative. A miss then takes the regular
    // common step as well; a hit returns at once from the intermediate point.
    if (shot.animeDelay >= 0) {
      --shot.animeDelay;
      shot.x += shot.vx;
      shot.y += shot.vy;
      if (outsideSpriteBounds(shot)) {
        shot.active = false;
      }
      // FUN_00405620 continues into its collision query even when the first
      // FUN_00406180 call just cleared the type. This is observable for a
      // shot fired from the transient x=386 edge position: it is culled at
      // x=384, then still hits a boss on that boundary in the same callback.
    }

    bool impacted = false;
    const SDL_Rect shotRect = collisionRect(shot);
    for (size_t enemyIndex = 0; enemyIndex < enemies_.size(); ++enemyIndex) {
      Entity& enemy = enemies_[enemyIndex];
      if (!enemy.active) continue;
      const int targetLeft = fixedToInt(enemy.x);
      const int targetTop = fixedToInt(enemy.y);
      const auto& targetBottomRight = enemyCachedBottomRight_[enemyIndex];
      if (shotRect.y > targetBottomRight[1] ||
          shotRect.x > targetBottomRight[0] ||
          targetLeft > shotRect.x + shotRect.w ||
          targetTop > shotRect.y + shotRect.h) continue;
      // The cached hitbox can still be found after an earlier shot in this
      // manager pass killed the boss. Its replacement NAnime callback ignores
      // event 0x100, although the shot still produces its own hit effects.
      if (enemy.hp >= 0) hitEnemy(enemy, &shot);
      impacted = true;
      break;
    }

    if (impacted) {
      spawnParticles(fixedToInt(shot.x) + 2, fixedToInt(shot.y) + 2, 8);
      playSound(2);
      shot.scriptPc = kScriptShotHit;
      if (!shot.piercing) {
        shot.collidable = false;
        shot.vx = 0;
        shot.vy = kOne;
      } else if (lives_ > 0) {
        // Once player +0x30 bit 0 is set on reaching the last stock, it stays
        // set after a score-based 1UP. FUN_00405620 then keeps the collision
        // callback, arms a three-tick delay and takes one common movement.
        shot.animeDelay = 3;
        shot.x += shot.vx;
        shot.y += shot.vy;
        if (outsideSpriteBounds(shot)) shot.active = false;
      } else {
        // With no stock in reserve the same persistent flag leaves the shot
        // in place and collidable, so it can hit the same object next tick.
      }
      continue;
    }

    shot.x += shot.vx;
    shot.y += shot.vy;
    if (outsideSpriteBounds(shot)) shot.active = false;
  }
}

void Gates32::updateEffects(bool bombPool) {
  std::vector<Entity>& pool = bombPool ? bombEffects_ : effects_;
  size_t encountered = 0;
  for (Entity& effect : pool) {
      if (!effect.active) continue;
      switch (effect.callback) {
        case EffectCallback::NAnime:
        // FUN_004056f0 calls FUN_00406080 even when the script just executed
        // opcode -1 and cleared the type. That final move survives in the
        // inactive slot and affects the fraction retained on later reuse.
        {
          bool impacted = false;
          if (bombPool && effect.collidable && effect.sprite >= 0 &&
              effect.sprite < static_cast<int>(sprites_.size())) {
            const SpriteInfo& sprite = sprites_[effect.sprite];
            const int left = fixedToInt(effect.x) - 2;
            const int top = fixedToInt(effect.y) - 2;
            const int right = left + sprite.width + 4;
            const int bottom = top + sprite.height + 4;
            for (size_t enemyIndex = 0; enemyIndex < enemies_.size(); ++enemyIndex) {
              Entity& enemy = enemies_[enemyIndex];
              if (!enemy.active) continue;
              // FUN_004066e0 reads the current left/top but the cached
              // absolute right/bottom written by FUN_00406230 after the
              // enemy's own manager callback. A bomb can replace a boss's
              // callback and shift its origin mid-pass, leaving these two
              // endpoints stale until the next enemy-manager update.
              const int targetLeft = fixedToInt(enemy.x);
              const int targetTop = fixedToInt(enemy.y);
              const auto& targetBottomRight = enemyCachedBottomRight_[enemyIndex];
              if (top > targetBottomRight[1] || left > targetBottomRight[0] ||
                  targetLeft > right || targetTop > bottom) continue;
              // Other bomb records in the same manager pass can still find a
              // boss after its callback was replaced by the death NAnime
              // callback. The collision produces particles, but event 0x100
              // is then a no-op and must not award score or kill it again.
              if (enemy.hp >= 0) hitEnemy(enemy, &effect);
              spawnParticles(fixedToInt(effect.x) + 2, fixedToInt(effect.y) + 2, 8);
              playSound(2);
              impacted = true;
              break;
            }
            if (!impacted) {
              for (Entity& bullet : enemyBullets_) {
                if (!bullet.active || !bullet.collidable) continue;
                const SDL_Rect target = collisionRect(bullet);
                if (top > target.y + target.h || left > target.x + target.w ||
                    target.x > right || target.y > bottom) continue;
                hitEnemyBullet(bullet);
                impacted = true;
                break;
              }
            }
          }

          if (impacted) {
            // LAB_004053e0 advances the animation and returns immediately.
            // The bomb is neither moved nor destroyed by the collision.
            advanceNAnime(effect);
            break;
          }

          const bool alive = advanceNAnime(effect);
          effect.x += effect.vx;
          effect.y += effect.vy;
          if (alive && outsideSpriteBounds(effect)) effect.active = false;
          break;
        }

        case EffectCallback::TrailEmitter: {
        if (effect.animeDelay < 0) effect.active = false;
        --effect.animeDelay;
        if ((effect.animeDelay & 3) == 0) {
          Entity* child = spawnAnime(fixedToInt(effect.x), fixedToInt(effect.y),
                                     kScriptTrail, effect.vy / 2, effect.vx / 2,
                                     effect.active ? 0 : 30);
          if (child) {
            if (!effect.active) child->sprite = runtimeI16(0x004165dc);
            // FUN_004092d0 applies the 0.9 decay only after a non-null child
            // allocation. A cached-count rejection leaves the velocity intact.
            effect.vx = static_cast<int32_t>(effect.vx * 0.9);
            effect.vy = static_cast<int32_t>(effect.vy * 0.9);
          }
        }
        effect.x += effect.vx;
        effect.y += effect.vy;
        // FUN_00409190 finishes with FUN_004060d0, whose point-like mover
        // clamps the integer word to the viewport and reflects velocity while
        // preserving the 16-bit coordinate fraction.
        int x = fixedToInt(effect.x);
        if (x < 0) {
          effect.x = withIntegerPart(effect.x, 0);
          effect.vx = -effect.vx;
        } else if (x >= kWidth) {
          effect.x = withIntegerPart(effect.x, kWidth - 1);
          effect.vx = -effect.vx;
        }
        int y = fixedToInt(effect.y);
        if (y < 0) {
          effect.y = withIntegerPart(effect.y, 0);
          effect.vy = -effect.vy;
        } else if (y >= kHeight) {
          effect.y = withIntegerPart(effect.y, kHeight - 1);
          effect.vy = -effect.vy;
        }
        break;
      }

      case EffectCallback::ExplosionEmitter: {
        if (effect.animeDelay < 0) effect.active = false;
        --effect.animeDelay;
        const int angle = static_cast<uint16_t>(randomSigned()) & 255;
        // FUN_004092d0 allocates before reading the emitter's script and
        // coordinates. On the final tick the just-deactivated emitter can be
        // the first free slot and is therefore cleared/reused as its own
        // child; the subsequent script read then observes zero and selects
        // kScriptExplosion. Keep that aliasing and evaluation order intact.
        Entity* child = allocate(effects_, EntityKind::Effect);
        if (child) {
          const uint32_t script = effect.scriptPc;
          const int x = fixedToInt(effect.x), y = fixedToInt(effect.y);
          child->x = withIntegerPart(child->x, x);
          child->y = withIntegerPart(child->y, y);
          child->animeDelay = 0;
          child->scriptPc = script ? script : kScriptExplosion;
          child->sprite = -1;
          child->callback = EffectCallback::NAnime;
          child->collidable = false;
          const int magnitude =
              (static_cast<uint16_t>(randomSigned()) & 0x7f) + 0x100;
          child->vx = (static_cast<int32_t>(sin16_[angle]) * magnitude) / 64;
          child->vy = (static_cast<int32_t>(cos16_[angle]) * magnitude) / 64;
        }
        break;
      }

      case EffectCallback::RectEmitter: {
        const uint32_t timer = static_cast<uint32_t>(--effect.animeDelay);
        if (effect.animeDelay < 0) effect.active = false;
        if ((timer & 3) != 0 && effect.frameFirst > 0 && effect.frameLast > 0) {
          // FUN_004094e0 allocates first. If the cached manager count rejects
          // the request, neither coordinate consumes a random value.
          Entity* child = allocate(effects_, EntityKind::Effect);
          if (child) {
            const int x = fixedToInt(effect.x) +
                          static_cast<uint16_t>(randomSigned()) % effect.frameFirst;
            const int y = fixedToInt(effect.y) +
                          static_cast<uint16_t>(randomSigned()) % effect.frameLast;
            child->x = withIntegerPart(child->x, x);
            child->y = withIntegerPart(child->y, y);
            child->animeDelay = 3;
            child->scriptPc = 0;
            child->sprite = -1;
            child->callback = EffectCallback::ExplosionEmitter;
            child->collidable = false;
          }
        }
        break;
      }

      case EffectCallback::RadialEmitter: {
        playSound(4);
        int angle = static_cast<uint8_t>(randomSigned());
        const int count = std::max(0, effect.aux);
        for (int i = 0; i < count; ++i) {
          Entity* child = spawnAnime(fixedToInt(effect.x), fixedToInt(effect.y), effect.scriptPc);
          if (!child) break;
          child->vx = static_cast<int32_t>(sin16_[angle & 255]) * effect.savedVx;
          child->vy = static_cast<int32_t>(cos16_[angle & 255]) * effect.savedVy;
          angle = (angle + (count ? 256 / count : 0)) & 255;
        }
        effect.active = false;
        break;
      }

      case EffectCallback::None:
        effect.x += effect.vx;
        effect.y += effect.vy;
        if (--effect.timer <= 0) {
          effect.timer = effect.frameDelay;
          if (++effect.sprite > effect.frameLast) effect.active = false;
        }
        break;
    }
    if (!effect.active) continue;
    const int x = fixedToInt(effect.x), y = fixedToInt(effect.y);
    if (x < -96 || x > kWidth + 96 || y < -96 || y > kHeight + 96) effect.active = false;
    if (effect.active) ++encountered;
  }
  if (bombPool) bombEffectManagerCount_ = encountered;
  else effectManagerCount_ = encountered;
}

void Gates32::updateParticles() {
  const size_t expected = particleManagerCount_;
  size_t encountered = 0;
  for (Entity& particle : particles_) {
    if (!particle.active) continue;
    ++encountered;
    const int x = fixedToInt(particle.x), y = fixedToInt(particle.y);
    if (x < 0 || x >= kWidth || y < 0 || y >= kHeight) {
      particle.active = false;
    } else {
      // qword 00415058 is -6553.6. The VC5 conversion truncates toward zero,
      // so positive velocities gain 6553 and sufficiently negative ones 6554.
      particle.vy = static_cast<int32_t>(static_cast<double>(particle.vy) + 6553.6);
      particle.x += particle.vx;
      particle.y += particle.vy;
    }
    if (encountered >= expected) break;
  }
  particleManagerCount_ = encountered;
}

uint64_t Gates32::stateHash(uint8_t input) const {
  uint64_t hash = 1469598103934665603ULL;
  auto mix32 = [&](int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    for (int shift = 0; shift < 32; shift += 8) {
      hash ^= static_cast<uint8_t>(bits >> shift);
      hash *= 1099511628211ULL;
    }
  };
  auto originalType = [](const Entity& entity) {
    switch (entity.kind) {
      case EntityKind::Enemy1: return 0x200;
      case EntityKind::Enemy2: return 0x201;
      case EntityKind::Gates1: return 0x301;
      case EntityKind::Gates2: return 0x302;
      case EntityKind::EnemyBullet: return 0x80;
      case EntityKind::EnemyBullet90: return 0x90;
      case EntityKind::PlayerShot:
      case EntityKind::Effect: return 0x90;
      case EntityKind::Particle: return -1;
      default: return 0;
    }
  };
  using ObjectState = std::array<int32_t, 8>;
  auto makeState = [&](const Entity& entity) -> ObjectState {
    return {originalType(entity), entity.x, entity.y, entity.vx, entity.vy,
            entity.sprite, entity.animeDelay,
            entity.scriptPc ? static_cast<int32_t>(entity.scriptPc - kRuntimeBase) : 0};
  };
  auto countActive = [](const std::vector<Entity>& pool) {
    return static_cast<int32_t>(std::count_if(pool.begin(), pool.end(),
                                              [](const Entity& entity) { return entity.active; }));
  };
  std::vector<ObjectState> world;
  std::vector<ObjectState> anime;
  std::vector<ObjectState> shots;
  std::vector<ObjectState> points;
  for (const Entity& entity : enemies_) if (entity.active) world.push_back(makeState(entity));
  for (const Entity& entity : enemyBullets_) if (entity.active) world.push_back(makeState(entity));
  for (const Entity& entity : effects_) if (entity.active) anime.push_back(makeState(entity));
  for (const Entity& entity : playerShots_) if (entity.active) shots.push_back(makeState(entity));
  for (const Entity& entity : particles_) if (entity.active) points.push_back(makeState(entity));
  for (auto* group : {&world, &anime, &shots, &points}) std::sort(group->begin(), group->end());

  mix32(input);
  mix32(randomIndex_);
  mix32(playerX_);
  mix32(playerY_);
  // The original trace address is a dword containing the adjacent 16-bit
  // lives and invulnerability fields.
  const int32_t packedLives = static_cast<int32_t>(
      (static_cast<uint32_t>(invulnerable_ & 0xffff) << 16) |
      static_cast<uint32_t>(lives_ & 0xffff));
  mix32(packedLives);
  mix32(score_);
  mix32(level_);
  mix32(countActive(enemies_) + countActive(enemyBullets_));
  mix32(countActive(effects_));
  mix32(countActive(playerShots_));
  mix32(countActive(particles_));
  for (const auto* group : {&world, &anime, &shots, &points})
    for (const ObjectState& object : *group)
      for (int32_t value : object) mix32(value);
  return hash;
}

void Gates32::appendStateTrace(uint8_t input) {
  if (!traceActive_ || frameNumber_ < traceFrameStart_) return;
  using ObjectState = std::array<int32_t, 8>;
  auto typeOf = [](const Entity& entity) {
    switch (entity.kind) {
      case EntityKind::Enemy1: return 0x200;
      case EntityKind::Enemy2: return 0x201;
      case EntityKind::Gates1: return 0x301;
      case EntityKind::Gates2: return 0x302;
      case EntityKind::EnemyBullet: return 0x80;
      case EntityKind::EnemyBullet90: return 0x90;
      default: return 0;
    }
  };
  auto stateOf = [&](const Entity& entity, int forcedType, bool point) {
    return ObjectState{
        forcedType, entity.x, entity.y, entity.vx, entity.vy,
        point ? -1 : entity.sprite, point ? 0 : entity.animeDelay,
        (!point && entity.scriptPc) ? static_cast<int32_t>(entity.scriptPc - kRuntimeBase) : 0};
  };
  std::vector<ObjectState> world, anime, shots, points;
  for (const auto* pool : {&enemies_, &enemyBullets_}) {
    for (const Entity& entity : *pool) {
      if (!entity.active) continue;
      world.push_back(stateOf(entity, typeOf(entity), false));
    }
  }
  for (const Entity& entity : effects_)
    if (entity.active) anime.push_back(stateOf(entity, 0x90, false));
  for (const Entity& entity : playerShots_)
    if (entity.active) shots.push_back(stateOf(entity, 0x90, false));
  for (const Entity& entity : particles_)
    if (entity.active) points.push_back(stateOf(entity, -1, true));
  for (auto* group : {&world, &anime, &shots, &points}) std::sort(group->begin(), group->end());

  const int32_t packedLives = static_cast<int32_t>(
      (static_cast<uint32_t>(invulnerable_ & 0xffff) << 16) |
      static_cast<uint32_t>(lives_ & 0xffff));
  uint64_t bombsHash = 1469598103934665603ULL;
  auto mixBomb = [&](int32_t value) {
    const uint32_t bits = static_cast<uint32_t>(value);
    for (int shift = 0; shift < 32; shift += 8) {
      bombsHash ^= static_cast<uint8_t>(bits >> shift);
      bombsHash *= 1099511628211ULL;
    }
  };
  for (size_t index = 0; index < bombEffects_.size(); ++index) {
    const Entity& effect = bombEffects_[index];
    if (!effect.active) continue;
    mixBomb(static_cast<int32_t>(index));
    for (int32_t value : stateOf(effect, 0x90, false)) mixBomb(value);
  }
  if (traceCompact_) {
    char row[512];
    std::snprintf(row, sizeof(row),
                  "%llu,%u,%d,%d,%d,%d,%d,%d,%zu,%zu,%zu,%zu,%zu,%d,%d,%d,%d,%zu,%zu,%016llx,%016llx\n",
                  static_cast<unsigned long long>(frameNumber_),
                  static_cast<unsigned>(input), randomIndex_, playerX_, playerY_,
                  packedLives, score_, level_, world.size(), anime.size(),
                  shots.size(), points.size(), enemyManagerCount_, stageState_,
                  stageCounter_, stageCountdown_, stagePeriod_, effectManagerCount_,
                  particleManagerCount_,
                  static_cast<unsigned long long>(bombsHash),
                  static_cast<unsigned long long>(stateHash(input)));
    stateTrace_ += row;
    return;
  }

  auto stateText = [](const ObjectState& state) {
    std::string result;
    for (size_t i = 0; i < state.size(); ++i) {
      if (i) result += ':';
      result += std::to_string(state[i]);
    }
    return result;
  };
  auto allText = [&](const std::vector<ObjectState>& group) {
    if (group.empty()) return std::string("-");
    std::string result;
    for (const ObjectState& state : group) {
      if (!result.empty()) result += '|';
      result += stateText(state);
    }
    return result;
  };
  auto headText = [&](const std::vector<ObjectState>& group) {
    return group.empty() ? std::string("-") : stateText(group.front());
  };
  char prefix[1024];
  std::snprintf(prefix, sizeof(prefix),
                "%llu,%u,%d,%d,%d,%d,%d,%d,%zu,%zu,%zu,%zu,%s,%s,%s,%s,",
                static_cast<unsigned long long>(frameNumber_), static_cast<unsigned>(input),
                randomIndex_, playerX_, playerY_, packedLives, score_, level_,
                world.size(), anime.size(), shots.size(), points.size(),
                headText(world).c_str(), headText(anime).c_str(),
                headText(shots).c_str(), headText(points).c_str());
  stateTrace_ += prefix;
  for (const auto* group : {&world, &anime, &shots, &points}) {
    stateTrace_ += allText(*group);
    stateTrace_ += ',';
  }
  std::string shotsMeta;
  for (size_t index = 0; index < playerShots_.size(); ++index) {
    const Entity& shot = playerShots_[index];
    if (!shot.active) continue;
    if (!shotsMeta.empty()) shotsMeta += '|';
    shotsMeta += std::to_string(index) + ':' +
                 std::to_string(shot.collidable ? 1 : 0) + ':' +
                 std::to_string(shot.piercing ? 1 : 0) + ':' +
                 std::to_string(static_cast<int>(shot.callback)) + ':' +
                 std::to_string(shot.x) + ':' +
                 std::to_string(shot.y) + ':' +
                 std::to_string(shot.animeDelay) + ':' +
                 std::to_string(shot.scriptPc
                                    ? static_cast<int32_t>(shot.scriptPc - kRuntimeBase)
                                    : 0);
  }
  stateTrace_ += shotsMeta.empty() ? "-," : shotsMeta + ',';
  std::string worldSlots;
  auto appendWorldSlots = [&](const std::vector<Entity>& pool, int poolId) {
    for (size_t index = 0; index < pool.size(); ++index) {
      const Entity& entity = pool[index];
      if (!entity.active) continue;
      if (!worldSlots.empty()) worldSlots += '|';
      worldSlots += std::to_string(poolId) + ':' + std::to_string(index) + ':' +
                    stateText(stateOf(entity, typeOf(entity), false));
    }
  };
  appendWorldSlots(enemyBullets_, 0);
  appendWorldSlots(enemies_, 1);
  stateTrace_ += worldSlots.empty() ? "-," : worldSlots + ',';
  std::string bombsAll;
  for (size_t index = 0; index < bombEffects_.size(); ++index) {
    const Entity& effect = bombEffects_[index];
    if (!effect.active) continue;
    if (!bombsAll.empty()) bombsAll += '|';
    bombsAll += std::to_string(index) + ':' +
                stateText(stateOf(effect, 0x90, false));
  }
  stateTrace_ += bombsAll.empty() ? "-," : bombsAll + ',';
  stateTrace_ += std::to_string(enemyManagerCount_) + ',';
  stateTrace_ += std::to_string(stageState_) + ',';
  stateTrace_ += std::to_string(stageCounter_) + ',';
  stateTrace_ += std::to_string(stageCountdown_) + ',';
  stateTrace_ += std::to_string(stagePeriod_) + ',';
  stateTrace_ += std::to_string(effectManagerCount_) + ',';
  stateTrace_ += std::to_string(particleManagerCount_) + ',';
  char suffix[64];
  std::snprintf(suffix, sizeof(suffix), "%016llx,%016llx\n",
                static_cast<unsigned long long>(bombsHash),
                static_cast<unsigned long long>(stateHash(input)));
  stateTrace_ += suffix;
}

void Gates32::publishStateTrace() {
  if (stateTrace_.empty()) return;
#ifdef __EMSCRIPTEN__
  EM_ASM({
    window.gates32StateTrace = UTF8ToString($0);
    var traceNode = document.getElementById('gates32-state-trace');
    if (!traceNode) {
      traceNode = document.createElement('script');
      traceNode.id = 'gates32-state-trace';
      traceNode.type = 'text/csv';
      traceNode.hidden = true;
      document.body.appendChild(traceNode);
    }
    traceNode.textContent = window.gates32StateTrace;
    if (window.gates32Diagnostics) {
      window.gates32Diagnostics.traceRows = Math.max(0, window.gates32StateTrace.split('\n').length - 2);
      window.gates32Diagnostics.traceReady = true;
    }
    var status = document.getElementById('status');
    if (status) {
      status.dataset.traceReady = 'true';
      status.dataset.traceRows = String(Math.max(0, window.gates32StateTrace.split('\n').length - 2));
    }
  }, stateTrace_.c_str());
#endif
}

void Gates32::saveStateTrace() {
  if (stateTrace_.empty()) return;
  std::ofstream output("/gates32-state.csv", std::ios::binary);
  output.write(stateTrace_.data(), static_cast<std::streamsize>(stateTrace_.size()));
  output.close();
#ifdef __EMSCRIPTEN__
  EM_ASM({
    const data = FS.readFile('/gates32-state.csv');
    const blob = new Blob([data], {type: 'text/csv'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'gates32-web-state.csv';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
#endif
}

bool Gates32::outsideSpriteBounds(const Entity& entity) const {
  if (entity.sprite < 0 || entity.sprite >= static_cast<int>(sprites_.size())) return false;
  const SpriteInfo& sprite = sprites_[entity.sprite];
  if (!sprite.exists) return false;
  const int x = fixedToInt(entity.x), y = fixedToInt(entity.y);
  // FUN_00406140 removes a free-moving sprite once its full bitmap has left
  // the left/top edge, or its origin reaches the right/bottom edge.
  return sprite.width + x < 1 || x > kWidth - 1 ||
         sprite.height + y < 1 || y >= kHeight;
}

SDL_Rect Gates32::collisionRect(const Entity& entity) const {
  if (entity.sprite < 0 || entity.sprite >= static_cast<int>(sprites_.size())) return {0, 0, 0, 0};
  const SpriteInfo& sprite = sprites_[entity.sprite];
  // FUN_00406670 uses +0x10/+0x14 as inclusive right/bottom extents.
  // Zero is a deliberate point/line hitbox, not a request for image size.
  return {fixedToInt(entity.x), fixedToInt(entity.y),
          sprite.collisionRight, sprite.collisionBottom};
}

bool Gates32::intersects(const Entity& a, const Entity& b) const {
  const SDL_Rect ra = collisionRect(a), rb = collisionRect(b);
  // FUN_004066e0 compares inclusive right/bottom endpoints.
  return ra.y <= rb.y + rb.h && ra.x <= rb.x + rb.w &&
         rb.x <= ra.x + ra.w && rb.y <= ra.y + ra.h;
}

bool Gates32::intersectsPlayer(const Entity& entity) const {
  SDL_Rect player{fixedToInt(playerX_), fixedToInt(playerY_), 4, 4};
  const SDL_Rect other = collisionRect(entity);
  // FUN_004066e0 treats both right and bottom coordinates as inclusive.
  return player.y <= other.y + other.h && player.x <= other.x + other.w &&
         other.x <= player.x + player.w && other.y <= player.y + player.h;
}

void Gates32::update() {
  if (screen_ == Screen::Title) { updateTitle(); return; }
  const Screen screenAtStart = screen_;

  // Replay/live input is fetched inside the original player callback, after
  // its stock-range guard. A dead (-1) or cleared (24+) player therefore no
  // longer advances the replay cursor during the end-state grace period.
  const uint8_t input = (lives_ >= 0 && lives_ < 24) ? readInputMask() : 0;
  if (replayAbortRequested_) {
    replayAbortRequested_ = false;
    replayPlayback_ = false;
    screen_ = Screen::Title;
    titleTickAccumulator_ = 0;
    titleIdleCounter_ = 0;
    titlePreviousInput_ = readLiveInputMask();
#ifdef __EMSCRIPTEN__
    EM_ASM({
      if (window.gates32Diagnostics) window.gates32Diagnostics.screen = 'title';
    });
#endif
    return;
  }
  updateEnemies();
  // FUN_004064a0 calls FUN_00406230 after each enemy callback. These cached
  // absolute right/bottom endpoints remain unchanged for the rest of the
  // root-manager pass, even when a player shot later kills and shifts a boss
  // before the bomb child manager performs its collision queries.
  for (size_t i = 0; i < enemies_.size(); ++i) {
    const SDL_Rect rect = collisionRect(enemies_[i]);
    enemyCachedBottomRight_[i] = {rect.x + rect.w, rect.y + rect.h};
  }
  updateParticles();
  updateEnemyBullets();
  updateEffects();
  updatePlayer(input);
  updatePlayerShots();
  // The bomb records live in the player's child manager. Newly spawned bomb
  // objects receive their first callback in this same player-update pass.
  updateEffects(true);
  updateStage();
  ++frameNumber_;
  appendStateTrace(input);
  if (traceFrameLimit_ != 0 && frameNumber_ >= traceFrameLimit_) {
    publishTraceAfterFrame_ = true;
  }
  if (publishTraceAfterFrame_) {
    publishTraceAfterFrame_ = false;
    replayPlayback_ = false;
    traceActive_ = false;
    publishStateTrace();
  }

  if (screenAtStart == Screen::Playing) {
    if (lives_ < 0) {
      // The original switches state but keeps running every world/player/
      // stage manager for another 56 ticks (deadline = frame + 55, tested
      // with a strict less-than). Bombs can continue destroying and scoring.
      screen_ = Screen::GameOver;
      endTimer_ = 55;
      endStartFrame_ = frameNumber_;
    } else if (lives_ > 23 || score_ > 99999998 || level_ > 31) {
      screen_ = Screen::Clear;
      endTimer_ = 300;
      endStartFrame_ = frameNumber_;
    }
  } else if (screenAtStart == Screen::GameOver || screenAtStart == Screen::Clear) {
    if (--endTimer_ < 0) {
      // DAT_005a6368 receives the live player's recorded byte count here;
      // title-idle playback then reuses this in-memory recording.
      finishReplayRecording();
      // DAT_00592228 suppresses ranking writes for replay playback.
      if (!replaySession_) submitScore(screenAtStart == Screen::Clear);
      // FUN_00403f10 copies the just-rendered playfield into DAT_00551808
      // before returning to state 0. The title loop restores that buffer and
      // draws only the transparent title art and ranking over it.
      captureTitleBackground_ = true;
      screen_ = Screen::Title;
      titleTickAccumulator_ = 0;
      titleIdleCounter_ = 0;
      titlePreviousInput_ = 0;
#ifdef __EMSCRIPTEN__
      EM_ASM({
        if (window.gates32Diagnostics) window.gates32Diagnostics.screen = 'title';
      });
#endif
    }
  }
  if (screen_ != Screen::Playing && traceActive_ && !traceEndStates_) {
    replayPlayback_ = false;
    traceActive_ = false;
    publishTraceAfterFrame_ = false;
    publishStateTrace();
  }
}

void Gates32::drawSprite(int index, int x, int y) {
  if (index < 0 || index >= static_cast<int>(sprites_.size())) return;
  const SpriteInfo& info = sprites_[index];
  if (!info.exists) return;
  SDL_Rect src{info.atlasX, info.atlasY, info.width, info.height};
  SDL_Rect dst{x + info.offsetX, y + info.offsetY, info.width, info.height};
  SDL_RenderCopy(renderer_, spriteTexture_, &src, &dst);
}

void Gates32::drawText(SDL_Texture* texture, int cellHeight, int x, int y, const std::string& text) {
  int px = x;
  for (unsigned char ch : text) {
    if (ch == '\n') { px = x; y += cellHeight; continue; }
    // FUN_00408d80/FUN_00408e50: x=(code-1)&31, row=(code-0x21)>>5.
    if (ch > 32) {
      const int column = (static_cast<int>(ch) - 1) & 31;
      const int row = (static_cast<int>(ch) - 33) >> 5;
      SDL_Rect src{column * 8, row * cellHeight, 8, cellHeight};
      SDL_Rect dst{px, y, 8, cellHeight};
      SDL_RenderCopy(renderer_, texture, &src, &dst);
    }
    px += 8;
  }
}

void Gates32::drawText8(int x, int y, const std::string& text) { drawText(font8Texture_, 8, x, y, text); }
void Gates32::drawText16(int x, int y, const std::string& text) { drawText(font16Texture_, 16, x, y, text); }

void Gates32::renderTitle() {
  SDL_Rect dst{0, 0, kWidth, kHeight};
  if (titleBackgroundReady_ && titleBackgroundTexture_)
    SDL_RenderCopy(renderer_, titleBackgroundTexture_, nullptr, &dst);
  SDL_RenderCopy(renderer_, titleTexture_, nullptr, &dst);
  drawText16(128, 180, "SCORE RANKING");
  for (size_t i = 0; i < ranking_.size(); ++i) {
    const int y = 206 + static_cast<int>(i) * 18;
    char text[32];
    if (i == 0) {
      // Original string at 0041652c is "Top{|": the same two special
      // string16.bmp glyphs used to assemble the chick life icon.
      drawText16(64, y, "Top{|");
    } else {
      std::snprintf(text, sizeof(text), "No.%02d", static_cast<int>(i + 1));
      drawText16(64, y, text);
    }
    const ScoreEntry& entry = ranking_[i];
    if (entry.level < 1) {
      drawText16(112, y, "---------");
      continue;
    }
    std::snprintf(text, sizeof(text), "%8d0", entry.score);
    drawText16(112, y, text);
    std::snprintf(text, sizeof(text), "LV%2d", entry.level);
    drawText8(192, y, text);
    std::snprintf(text, sizeof(text), "TIME%7d", entry.time);
    drawText8(232, y, text);
    if ((entry.flags & 1) != 0) drawText8(192, y + 8, "CLEARED!!!!");
  }
  if (acchohMode_) drawText8(0, 376, "ACCHOH");
  if (explosionMode_) drawText8(0, 384, "EXPLODE");
  if (!soundEnabled_) drawText8(0, 392, "SOUND DISABLE");
}

void Gates32::renderGame() {
  const int px = fixedToInt(playerX_), py = fixedToInt(playerY_);
  if (backgroundEnabled_) {
    SDL_Rect mapSrc{std::clamp(px / 6, 0, 128), 512 + std::clamp(py / 6, 0, 624), kWidth, kHeight};
    SDL_Rect whole{0, 0, kWidth, kHeight};
    SDL_RenderCopy(renderer_, backTexture_, &mapSrc, &whole);
    SDL_Rect earthSrc{0, 0, 512, 512};
    SDL_Rect earthDst{64 - px / 2, 64 - py / 2, 512, 512};
    SDL_RenderCopy(renderer_, earthTexture_, &earthSrc, &earthDst);
  }

  for (const Entity& enemy : enemies_) if (enemy.active) drawSprite(enemy.sprite, fixedToInt(enemy.x), fixedToInt(enemy.y));
  for (const Entity& effect : effects_) if (effect.active) drawSprite(effect.sprite, fixedToInt(effect.x), fixedToInt(effect.y));
  for (const Entity& effect : bombEffects_) if (effect.active) drawSprite(effect.sprite, fixedToInt(effect.x), fixedToInt(effect.y));
  // The dedicated type-4 pool writes palette index 15 directly to the old
  // 8-bit framebuffer. Its palette entry is white in gates32.npk.
  SDL_SetRenderDrawColor(renderer_, 255, 255, 255, 255);
  for (const Entity& particle : particles_)
    if (particle.active) SDL_RenderDrawPoint(renderer_, fixedToInt(particle.x), fixedToInt(particle.y));
  for (const Entity& bullet : enemyBullets_) if (bullet.active) drawSprite(bullet.sprite, fixedToInt(bullet.x), fixedToInt(bullet.y));
  for (const Entity& shot : playerShots_) if (shot.active) drawSprite(shot.sprite, fixedToInt(shot.x), fixedToInt(shot.y));
  if (screen_ == Screen::Playing && (invulnerable_ == 0 || (invulnerable_ & 2) == 0)) drawSprite(1, px, py);

  // The original does not clear a dedicated HUD band. Backgrounds and
  // sprites remain visible here; only each opaque font cell overwrites them.
  char text[96];
  std::snprintf(text, sizeof(text), "HI %08d0 SCORE %08d0", highScore_, score_);
  drawText16(0, 0, text);
  int bulletCount = 0;
  for (const Entity& bullet : enemyBullets_) if (bullet.active) ++bulletCount;
  std::snprintf(text, sizeof(text), "FRAME RATE %02dFPS", 1000 / tickMs_);
  drawText8(240, 0, text);
  std::snprintf(text, sizeof(text), "E.GUN%03d", bulletCount);
  drawText8(240, 8, text);
  std::snprintf(text, sizeof(text), "LV%02d", level_);
  drawText8(328, 8, text);
  // FUN_00408b70 builds one 16x16 chick from the adjacent '{' and '|'
  // special glyphs in string16.bmp, starting at row 16 and advancing 16 px.
  for (int i = 0; i < std::max(0, lives_); ++i) drawText16(i * 16, 16, "{|");
  if (!soundEnabled_) drawText8(312, 16, "SOUND OFF");
  // State 2 (game over) has no overlay in FUN_00403f10: the live playfield
  // simply continues for its 55-count grace period. State 3 draws these three
  // original completion messages on top of the still-updating playfield.
  if (screen_ == Screen::Clear) {
    drawText16(116, 128, " Congratulation !!");
    std::snprintf(text, sizeof(text), "Clear Time %lluCount!!",
                  static_cast<unsigned long long>(endStartFrame_));
    drawText16(108, 144, text);
    drawText16(116, 160, "You are Kichigai !!");
  }
}

void Gates32::captureTitleBackground() {
  captureTitleBackground_ = false;
  if (!titleBackgroundTexture_ || SDL_SetRenderTarget(renderer_, titleBackgroundTexture_) != 0)
    return;

  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  // screen_ is already Title here, which deliberately suppresses both the
  // dead player and the clear-message overlay on the saved final frame.
  renderGame();
  titleBackgroundReady_ = true;
  SDL_SetRenderTarget(renderer_, nullptr);
#ifdef __EMSCRIPTEN__
  EM_ASM({
    var status = document.getElementById('status');
    if (status) status.dataset.titleBackgroundCaptured = 'true';
  });
#endif
}

void Gates32::render() {
  if (captureTitleBackground_) captureTitleBackground();
  SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
  SDL_RenderClear(renderer_);
  if (screen_ == Screen::Title) renderTitle(); else renderGame();
  SDL_RenderPresent(renderer_);
}

void Gates32::loadBundledReplay() {
  const char* replayPath = "/assets/original/gates32.rep";
#ifdef __EMSCRIPTEN__
  if (EM_ASM_INT({
        return new URLSearchParams(location.search).get('replay') === 'clear-lv32' ? 1 : 0;
      })) {
    replayPath = "/assets/original/gates32_clear_lv32.rep";
  }
#endif
  std::ifstream input(replayPath, std::ios::binary);
  if (!input) return;
  input.read(reinterpret_cast<char*>(&replayHeader_), sizeof(replayHeader_));
  replayInput_.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  if (replayHeader_.length && replayHeader_.length < replayInput_.size()) replayInput_.resize(replayHeader_.length);
}

void Gates32::saveReplay() {
  if (!replayRecording_ && replayInput_.empty()) return;
  replayHeader_.length = static_cast<uint32_t>(replayInput_.size());
  std::ofstream output("/gates32.rep", std::ios::binary);
  output.write(reinterpret_cast<const char*>(&replayHeader_), sizeof(replayHeader_));
  output.write(reinterpret_cast<const char*>(replayInput_.data()), static_cast<std::streamsize>(replayInput_.size()));
  output.close();
#ifdef __EMSCRIPTEN__
  EM_ASM({
    const data = FS.readFile('/gates32.rep');
    const blob = new Blob([data], {type: 'application/octet-stream'});
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'gates32.rep';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  });
#endif
}

void Gates32::browserFrame() {
  handleEvents();
  const uint32_t now = SDL_GetTicks();
  accumulator_ += std::min<uint32_t>(now - lastBrowserTime_, 250);
  lastBrowserTime_ = now;
  while (accumulator_ >= static_cast<uint32_t>(tickMs_)) {
    update();
    accumulator_ -= static_cast<uint32_t>(tickMs_);
  }
  render();
#ifndef __EMSCRIPTEN__
  if (!running_) return;
#endif
}

} // namespace

int main(int, char**) {
  static Gates32 game;
  if (!game.initialize()) {
    SDL_Log("Initialization failed: %s", SDL_GetError());
    return 1;
  }
#ifdef __EMSCRIPTEN__
  emscripten_set_main_loop_arg([](void* arg) { static_cast<Gates32*>(arg)->browserFrame(); }, &game, 0, 1);
#else
  // Native builds are useful for deterministic debugging.
  for (;;) {
    game.browserFrame();
    SDL_Delay(1);
  }
#endif
  game.shutdown();
  return 0;
}
