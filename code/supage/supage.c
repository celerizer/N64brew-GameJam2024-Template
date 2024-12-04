#include <libdragon.h>

#include <t3d/t3d.h>
#include <t3d/t3dmath.h>
#include <t3d/t3dmodel.h>
#include <t3d/t3dskeleton.h>
#include <t3d/t3danim.h>
#include <t3d/t3ddebug.h>

#include "../../core.h"
#include "../../minigame.h"

const MinigameDef minigame_def =
{
  .gamename = "Spider Game",
  .developername = "Keith Bourdon",
  .description = "A work-in-progress game where you play as a spider.",
  .instructions = "Pull yourself around the screen to collect points."
};

#define SUPAGE_MAX_PLAYERS 4

#define SUPAGE_INPUT_DEADZONE 5

/**
 * The joystick absolute value minimum that registers as the beginning of
 * a pull event
 */
#define SUPAGE_INPUT_PULL_MIN 20

/**
 * The joystick absolute value maximum value for a pull event
 */
#define SUPAGE_INPUT_PULL_MAX 60

#define SUPAGE_INPUT_PULL_RANGE (SUPAGE_INPUT_PULL_MAX - SUPAGE_INPUT_PULL_MIN)

#define SUPAGE_BALLS_PER_PLAYER 3

enum
{
  SPG_FONT_INVALID = 0,

  SPG_FONT_TEXT,
  SPG_FONT_TEXT_SHADOW,

  SPG_FONT_SIZE
};

enum
{
  SPG_SFX_DUMMY = 0,

  SPG_SFX_START,

  SPG_SFX_SIZE
};

typedef enum
{
  SPG_STATE_INVALID = 0,

  SPG_STATE_INIT,
  SPG_STATE_INSTRUCTIONS,
  SPG_STATE_INTRO,
  SPG_STATE_PLAYING,
  SPG_STATE_ENDING,
  SPG_STATE_CLEANUP,

  SPG_STATE_SIZE
} supage_state;

typedef enum
{
  SPG_PLAYER_STATE_INVALID = 0,

  SPG_PLAYER_STATE_IDLE,
  SPG_PLAYER_STATE_PULL_START,
  SPG_PLAYER_STATE_PULL_ACTIVE,
  SPG_PLAYER_STATE_PULL_RELEASE,
  SPG_PLAYER_STATE_MOVING,

  SPG_PLAYER_STATE_SIZE
} supage_player_state;

typedef enum
{
  SUPAGE_DIRECTION_NONE = 0,

  SUPAGE_DIRECTION_CEILING,
  SUPAGE_DIRECTION_FLOOR,
  SUPAGE_DIRECTION_WEST, // o|
  SUPAGE_DIRECTION_EAST, // |o

  SUPAGE_DIRECTION_SIZE
} supage_direction;

typedef struct
{
  float x;
  float y;
  float velocity_x;
  float velocity_y;
  int8_t pull;
  unsigned points;
  unsigned points_display;
  color_t color;
  bool is_human;
  supage_player_state state;

  /** From which direction the player is attached to a wall */
  supage_direction attach;

  joypad_inputs_t previous_inputs;

  unsigned no_act_frames;

  unsigned balls;
} supage_player_t;

typedef struct
{
  float x;
  float y;
  float radius;
  color_t color;
  unsigned owner;
} supage_ball_t;

typedef struct
{
  rdpq_font_t *fonts[SPG_FONT_SIZE];
  wav64_t sounds[SPG_SFX_SIZE];
  supage_player_t players[SUPAGE_MAX_PLAYERS];
  supage_ball_t balls[SUPAGE_MAX_PLAYERS * SUPAGE_BALLS_PER_PLAYER];
  unsigned ball_count;
  supage_state state;

  surface_t *depth_buffer;
  T3DViewport viewport;
} supage_ctx_t;

static supage_ctx_t supage;

void minigame_init()
{
  const color_t colors[] =
  {
    PLAYERCOLOR_1,
    PLAYERCOLOR_2,
    PLAYERCOLOR_3,
    PLAYERCOLOR_4,
  };
  unsigned i;

  /* Init game context to zeroes */
  memset(&supage, 0, sizeof(supage));

  supage.state = SPG_STATE_INIT;

  /* Init display */
#if SUPAGE_480P
  display_init(RESOLUTION_640x480, DEPTH_16_BPP, 3, GAMMA_NONE,
               FILTERS_RESAMPLE);
#else
  display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE,
               FILTERS_RESAMPLE);
#endif

  /* Init players */
  for (i = 0; i < SUPAGE_MAX_PLAYERS; i++)
  {
    supage.players[i].color = colors[i];
    supage.players[i].state = SPG_PLAYER_STATE_IDLE;
    if (core_get_playercount() > i)
      supage.players[i].is_human = true;
  }

  /* Init fonts */
  supage.fonts[SPG_FONT_TEXT] = rdpq_font_load_builtin(FONT_BUILTIN_DEBUG_VAR);
  rdpq_text_register_font(SPG_FONT_TEXT, supage.fonts[SPG_FONT_TEXT]);
  rdpq_text_register_font(SPG_FONT_TEXT_SHADOW, supage.fonts[SPG_FONT_TEXT]);

  /* Init sounds */
  wav64_open(&supage.sounds[SPG_SFX_START], "rom:/core/Start.wav64");

  /* Init graphics */
  t3d_init((T3DInitParams){});
  supage.depth_buffer = display_get_zbuf();
  supage.viewport = t3d_viewport_create();

  supage.state = SPG_STATE_PLAYING;
}

void supage_fixedloop_playing(float deltatime)
{
  supage_player_t *player;
  joypad_inputs_t inputs;
  unsigned i;

  /* Update players */
  for (i = 0; i < SUPAGE_MAX_PLAYERS; i++)
  {
    player = &supage.players[i];

    if (player->state == SPG_PLAYER_STATE_PULL_RELEASE)
    {
      player->no_act_frames--;
      if (player->no_act_frames == 0)
        player->state = SPG_PLAYER_STATE_IDLE;
      else
        continue;
    }

    if (player->is_human)
    {
      inputs = joypad_get_inputs(core_get_playercontroller(i));

      if (inputs.btn.start)
        minigame_end();

      /* Holding B, initiate a potential pull event */
      if (inputs.btn.b)
      {
        if (player->state == SPG_PLAYER_STATE_IDLE)
          player->state = SPG_PLAYER_STATE_PULL_START;
      }
      else
      {
        switch (player->state)
        {
        case SPG_PLAYER_STATE_PULL_START:
          player->state = SPG_PLAYER_STATE_IDLE;
          break;
        case SPG_PLAYER_STATE_PULL_ACTIVE:
          player->state = SPG_PLAYER_STATE_PULL_RELEASE;
          player->no_act_frames = 20;
          break;
        case SPG_PLAYER_STATE_IDLE:
          if (abs(inputs.stick_x) > SUPAGE_INPUT_DEADZONE)
            player->state = SPG_PLAYER_STATE_MOVING;
          break;
        case SPG_PLAYER_STATE_MOVING:
          player->velocity_x = inputs.stick_x * 0.05f;
          break;
        default:
          break;
        }
      }
    }
    else
    {
      /* AI */
    }

    player->x += supage.players[i].velocity_x * deltatime;
    player->y += supage.players[i].velocity_y * deltatime;

    /* Update displayed points */
    if (player->points_display < player->points)
      player->points_display++;
    else if (player->points_display > player->points)
      player->points_display--;
  }
}

void minigame_fixedloop(float deltatime)
{
  switch (supage.state)
  {
  case SPG_STATE_INIT:
    break;
  case SPG_STATE_INSTRUCTIONS:
    break;
  case SPG_STATE_INTRO:
    break;
  case SPG_STATE_PLAYING:
    supage_fixedloop_playing(deltatime);
    break;
  case SPG_STATE_ENDING:
    break;
  case SPG_STATE_CLEANUP:
    break;
  default:
    break;
  }
}

void minigame_loop(float deltatime)
{
  rdpq_attach(display_get(), supage.depth_buffer);
  rdpq_text_printf(NULL, SPG_FONT_TEXT, 155, 80, "State: %u", supage.state);
  /* Print debug info of the relevant player status */
  rdpq_text_printf(NULL, SPG_FONT_TEXT, 155, 100,
    "Player 1: %f %f %u",
    supage.players[0].x,
    supage.players[0].y,
    supage.players[0].state);
  rdpq_detach_show();
}

void minigame_cleanup()
{
  unsigned i;

  /* Cleanup sounds */
  for (i = 0; i < SPG_SFX_SIZE; i++)
    wav64_close(&supage.sounds[i]);

  /* Cleanup fonts */
  for (i = 1; i < SPG_FONT_SIZE; i++)
  {
    rdpq_text_unregister_font(i);
    if (supage.fonts[i])
      rdpq_font_free(supage.fonts[i]);
  }

  t3d_destroy();

  display_close();
}
