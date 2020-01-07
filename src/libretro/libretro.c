#include <stdint.h>

#include <libretro.h>

#include "mame.h"
#include "driver.h"
#include "state.h"
#include "log.h"

// Wrapper to build MAME on 3DS. It doesn't have stricmp.
#ifdef _3DS
int stricmp(const char *string1, const char *string2)
{
    return strcasecmp(string1, string2);
}
#endif

void mame_frame(void);
void mame_done(void);

#if defined(__CELLOS_LV2__) || defined(GEKKO) || defined(_XBOX)
unsigned activate_dcs_speedhack = 1;
#else
unsigned activate_dcs_speedhack = 0;
#endif

struct retro_perf_callback perf_cb;

retro_log_printf_t log_cb = NULL;
retro_video_refresh_t video_cb = NULL;
static retro_input_poll_t poll_cb = NULL;
static retro_input_state_t input_cb = NULL;
static retro_audio_sample_batch_t audio_batch_cb = NULL;
retro_environment_t environ_cb = NULL;

unsigned long lastled = 0;
retro_set_led_state_t led_state_cb = NULL;

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }
void retro_set_environment(retro_environment_t cb)
{
   static const struct retro_variable vars[] = {
      { "mame2003-frameskip", "Frameskip; 0|1|2|3|4|5" },
      { "mame2003-dcs-speedhack",
#if defined(__CELLOS_LV2__) || defined(GEKKO) || defined(_XBOX)
         "MK2/MK3 DCS Speedhack; disabled|enabled"
#else
         "MK2/MK3 DCS Speedhack; enabled|disabled"
#endif
      },
      { "mame2003-skip_disclaimer", "Skip Disclaimer; enabled|disabled" },
      { "mame2003-skip_warnings", "Skip Warnings; disabled|enabled" },
      { "mame2003-samples", "Samples; enabled|disabled" },
      { "mame2003-sample_rate", "Sample Rate (KHz); 48000|8000|11025|22050|44100" },
      { "mame2003-cheats", "Cheats; disabled|enabled" },
      { "mame2003-dialsharexy", "Share 2 player dial controls across one X/Y device; disabled|enabled" },
#if defined(__IOS__)
      { "mame2003-mouse_device", "Mouse Device; pointer|mouse|disabled" },
#else
      { "mame2003-mouse_device", "Mouse Device; mouse|pointer|disabled" },
#endif
      { "mame2003-rstick_to_btns", "Right Stick to Buttons; enabled|disabled" },
      { "mame2003-option_tate_mode", "TATE Mode; disabled|enabled" },
      { NULL, NULL },
   };
   environ_cb = cb;

   cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

#ifndef PATH_SEPARATOR
# if defined(WINDOWS_PATH_STYLE) || defined(_WIN32)
#  define PATH_SEPARATOR '\\'
# else
#  define PATH_SEPARATOR '/'
# endif
#endif

static char* normalizePath(char* aPath)
{
   char *tok;
   static const char replaced = (PATH_SEPARATOR == '\\') ? '/' : '\\';

   for (tok = strchr(aPath, replaced); tok; tok = strchr(aPath, replaced))
      *tok = PATH_SEPARATOR;

   return aPath;
}

static int getDriverIndex(const char* aPath)
{
    char driverName[128];
    char *path, *last;
    char *firstDot;
    int i;

    // Get all chars after the last slash
    path = normalizePath(strdup(aPath ? aPath : "."));
    last = strrchr(path, PATH_SEPARATOR);
    memset(driverName, 0, sizeof(driverName));
    strncpy(driverName, last ? last + 1 : path, sizeof(driverName) - 1);
    free(path);
    
    // Remove extension    
    firstDot = strchr(driverName, '.');

    if(firstDot)
       *firstDot = 0;

    // Search list
    for (i = 0; drivers[i]; i++)
    {
       if(strcmp(driverName, drivers[i]->name) == 0)
       {
          options.romset_filename_noext = strdup(driverName);         
          if (log_cb)
             log_cb(RETRO_LOG_INFO, "Found game: %s [%s].\n", driverName, drivers[i]->name);
          return i;
       }
    }
    
    return -1;
}

static char* peelPathItem(char* aPath)
{
    char* last = strrchr(aPath, PATH_SEPARATOR);
    if(last)
       *last = 0;
    
    return aPath;
}

static int driverIndex; //< Index of mame game loaded

//

extern const struct KeyboardInfo retroKeys[];
extern int retroKeyState[512];
extern int retroJsState[72];

extern int16_t mouse_x[4];
extern int16_t mouse_y[4];
int16_t prev_pointer_x;
int16_t prev_pointer_y;
extern int16_t analogjoy[4][4];

extern struct osd_create_params videoConfig;

unsigned retroColorMode;
int16_t XsoundBuffer[2048];
char *fallbackDir;
char *systemDir;
char *romDir;
char *saveDir;

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name = "MAME 2003";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version = "0.78" GIT_VERSION;
   info->valid_extensions = "zip";
   info->need_fullpath = true;   
   info->block_extract = true;
}

int sample_rate;

int frameskip;
int gotFrame;
unsigned skip_disclaimer = 0;
unsigned skip_warnings = 0;
unsigned samples = 0;
unsigned cheats = 0;
unsigned dial_share_xy = 0;
unsigned mouse_device = 0;
unsigned rstick_to_btns = 0;
unsigned option_tate_mode = 0;

static void update_variables(void)
{
   struct retro_led_interface ledintf;
   struct retro_variable var;

   var.value = NULL;
   var.key = "mame2003-frameskip";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
      frameskip = atoi(var.value);

   var.value = NULL;
   var.key = "mame2003-dcs-speedhack";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         activate_dcs_speedhack = 1;
      else
         activate_dcs_speedhack = 0;
   }
   else
      activate_dcs_speedhack = 0;

   var.value = NULL;
   var.key = "mame2003-skip_disclaimer";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         skip_disclaimer = 1;
      else
         skip_disclaimer = 0;
   }
   else
      skip_disclaimer = 0;

   var.value = NULL;
   var.key = "mame2003-skip_warnings";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         skip_warnings = 1;
      else
         skip_warnings = 0;
   }
   else
      skip_warnings = 0;

   var.value = NULL;
   var.key = "mame2003-samples";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         samples = 1;
      else
         samples = 0;
   }
   else
      samples = 0;

   var.value = NULL;
   var.key = "mame2003-sample_rate";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      options.samplerate = atoi(var.value);
   }
   else
      options.samplerate = 48000;

   var.value = NULL;
   var.key = "mame2003-cheats";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         cheats = 1;
      else
         cheats = 0;
   }
   else
      cheats = 0;

   var.value = NULL;
   var.key = "mame2003-dialsharexy";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         dial_share_xy = 1;
      else
         dial_share_xy = 0;
   }
   else
      dial_share_xy = 0;

   var.value = NULL;
   var.key = "mame2003-mouse_device";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "pointer") == 0)
         mouse_device = RETRO_DEVICE_POINTER;
      else if(strcmp(var.value, "mouse") == 0)
         mouse_device = RETRO_DEVICE_MOUSE;
      else
         mouse_device = 0;
   }
   else
      mouse_device = 0;

   var.value = NULL;
   var.key = "mame2003-rstick_to_btns";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         rstick_to_btns = 1;
      else
         rstick_to_btns = 0;
   }
   else
      rstick_to_btns = 0;

   var.value = NULL;
   var.key = "mame2003-option_tate_mode";

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || var.value)
   {
      if(strcmp(var.value, "enabled") == 0)
         option_tate_mode = 1;
      else
         option_tate_mode = 0;
   }
   else
      option_tate_mode = 0;


   ledintf.set_led_state = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &ledintf))
      led_state_cb = ledintf.set_led_state;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
  mame2003_video_get_geometry(&info->geometry);
  info->timing.fps = Machine->drv->frames_per_second;
  if ( Machine->drv->frames_per_second * 1000 < options.samplerate)
    info->timing.sample_rate = 22050;

  else 
    info->timing.sample_rate = options.samplerate;
}

static void check_system_specs(void)
{
   // TODO - set variably
   // Midway DCS - Mortal Kombat/NBA Jam etc. require level 9
   unsigned level = 10;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_init (void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

#ifdef LOG_PERFORMANCE
   environ_cb(RETRO_ENVIRONMENT_GET_PERF_INTERFACE, &perf_cb);
#endif

   update_variables();
   check_system_specs();
}

void retro_deinit(void)
{
#ifdef LOG_PERFORMANCE
   perf_cb.perf_log();
#endif
}

void retro_reset (void)
{
    machine_reset();
}

/* get pointer axis vector from coord */
int16_t get_pointer_delta(int16_t coord, int16_t *prev_coord)
{
   int16_t delta = 0;
   if (*prev_coord == 0 || coord == 0)
   {
      *prev_coord = coord;
   }
   else
   {
      if (coord != *prev_coord)
      {
         delta = coord - *prev_coord;
         *prev_coord = coord;
      }
   }
   
   return delta;
}

void retro_run (void)
{
   int i;
   bool pointer_pressed;
   const struct KeyboardInfo *thisInput;
   bool updated = false;

   poll_cb();

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables();

   /* Keyboard */
   thisInput = retroKeys;
   while(thisInput->name)
   {
      retroKeyState[thisInput->code] = input_cb(0, RETRO_DEVICE_KEYBOARD, 0, thisInput->code);
      thisInput ++;
   }
   
   for (i = 0; i < 4; i ++)
   {
      unsigned int offset = (i * 18);

      /* Analog joystick */
      analogjoy[i][0] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
      analogjoy[i][1] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
      analogjoy[i][2] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
      analogjoy[i][3] = input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
      
      /* Joystick */
      if (rstick_to_btns)
      {
         /* if less than 0.5 force, ignore and read buttons as usual */
         retroJsState[RETRO_DEVICE_ID_JOYPAD_B + offset] = analogjoy[i][3] >  0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
         retroJsState[RETRO_DEVICE_ID_JOYPAD_Y + offset] = analogjoy[i][2] < -0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
      }
      else
      {
         retroJsState[ RETRO_DEVICE_ID_JOYPAD_B + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B);
         retroJsState[ RETRO_DEVICE_ID_JOYPAD_Y + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y);
      }
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_SELECT + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_START + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_UP + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_DOWN + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_LEFT + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_RIGHT + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT);
      if (rstick_to_btns)
      {
         retroJsState[ RETRO_DEVICE_ID_JOYPAD_A + offset] = analogjoy[i][2] >  0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
         retroJsState[ RETRO_DEVICE_ID_JOYPAD_X + offset] = analogjoy[i][3] < -0x4000 ? 1 : input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
      }
      else
      {
         retroJsState[ RETRO_DEVICE_ID_JOYPAD_A + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A);
         retroJsState[ RETRO_DEVICE_ID_JOYPAD_X + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X);
      }
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_L + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_R + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_L2 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_R2 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_L3 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3);
      retroJsState[ RETRO_DEVICE_ID_JOYPAD_R3 + offset] = input_cb(i, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3);
      
      if (mouse_device)
      {
         if (mouse_device == RETRO_DEVICE_MOUSE)
         {
            retroJsState[16 + offset] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
            retroJsState[17 + offset] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);
            mouse_x[i] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
            mouse_y[i] = input_cb(i, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);
         }
         else // RETRO_DEVICE_POINTER
         {
            pointer_pressed = input_cb(i, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_PRESSED);
            retroJsState[16 + offset] = pointer_pressed;
            retroJsState[17 + offset] = 0; // padding
            mouse_x[i] = pointer_pressed ? get_pointer_delta(input_cb(i, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_X), &prev_pointer_x) : 0;
            mouse_y[i] = pointer_pressed ? get_pointer_delta(input_cb(i, RETRO_DEVICE_POINTER, 0, RETRO_DEVICE_ID_POINTER_Y), &prev_pointer_y) : 0;
         }
      }
      else
      {
         retroJsState[16 + offset] = 0;
         retroJsState[17 + offset] = 0;
      }
   }

   mame_frame();
}


bool retro_load_game(const struct retro_game_info *game)
{
   if (!game)
      return false;

    // Find game index
    driverIndex = getDriverIndex(game->path);
    
    if(driverIndex)
    {
        #define describe_buttons(INDEX) \
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "Joystick Left" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Joystick Right" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "Joystick Up" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "Joystick Down" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "Button 1" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "Button 2" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "Button 3" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Button 4" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Button 5" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Button 6" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "Button 7" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "Button 8" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "Button 9" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "Button 10" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,   "Insert Coin" },\
        { INDEX, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" },

        struct retro_input_descriptor desc[] = {
            describe_buttons(0)
            describe_buttons(1)
            describe_buttons(2)
            describe_buttons(3)
            { 0, 0, 0, 0, NULL }
            };

        fallbackDir = strdup(game->path);
        
        /* Get system directory from frontend */
        environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY,&systemDir);
        if (systemDir == NULL || systemDir[0] == '\0')
        {
            /* if non set, use old method */
            systemDir = normalizePath(fallbackDir);
            systemDir = peelPathItem(systemDir);
        }
        
        /* Get save directory from frontend */
        environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY,&saveDir);
        if (saveDir == NULL || saveDir[0] == '\0')
        {
            /* if non set, use old method */
            saveDir = normalizePath(fallbackDir);
            saveDir = peelPathItem(saveDir);
        }

        // Get ROM directory
        romDir = normalizePath(fallbackDir);
        romDir = peelPathItem(romDir);

    
        
    
        // Set all options before starting the game
        options.vector_resolution_multiplier = 2; 
        options.antialias = 1; // 1 or 0
        options.beam = 2; //use 2.f  if using a decimal point 1|1.2|1.4|1.6|1.8|2|2.5|3|4|5|6|7|8|9|10|11|12 only works with antialas on
        options.translucency = 1; //integer: 1 to enable translucency on vectors
        options.vector_intensity = 1.5f; // 0.5|1.5|1|2|2.5|3
        options.vector_flicker = (int)(2.55 * 1.5f); // |0.5|1|1.5|2|2.5|3

        options.skip_disclaimer = skip_disclaimer;
        options.skip_warnings = skip_warnings;
        options.use_samples = samples;
        options.cheat = cheats;

        environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, desc);

        // Boot the emulator
        return run_game(driverIndex) == 0;
    }
    else
    {
        return false;
    }
}

void retro_unload_game(void)
{
    mame_done();
    
    free(fallbackDir);
    systemDir = 0;
}

size_t retro_serialize_size(void)
{
    extern size_t state_get_dump_size(void);
    
    return state_get_dump_size();
}

bool retro_serialize(void *data, size_t size)
{
   int cpunum;
	if(retro_serialize_size() && data && size)
	{
		/* write the save state */
		state_save_save_begin(data);

		/* write tag 0 */
		state_save_set_current_tag(0);
		if(state_save_save_continue())
		{
		    return false;
		}

		/* loop over CPUs */
		for (cpunum = 0; cpunum < cpu_gettotalcpu(); cpunum++)
		{
			cpuintrf_push_context(cpunum);

			/* make sure banking is set */
			activecpu_reset_banking();

			/* save the CPU data */
			state_save_set_current_tag(cpunum + 1);
			if(state_save_save_continue())
			    return false;

			cpuintrf_pop_context();
		}

		/* finish and close */
		state_save_save_finish();
		
		return true;
	}

	return false;
}

bool retro_unserialize(const void * data, size_t size)
{
    int cpunum;
	/* if successful, load it */
	if (retro_serialize_size() && data && size && !state_save_load_begin((void*)data, size))
	{
        /* read tag 0 */
        state_save_set_current_tag(0);
        if(state_save_load_continue())
            return false;

        /* loop over CPUs */
        for (cpunum = 0; cpunum < cpu_gettotalcpu(); cpunum++)
        {
            cpuintrf_push_context(cpunum);

            /* make sure banking is set */
            activecpu_reset_banking();

            /* load the CPU data */
            state_save_set_current_tag(cpunum + 1);
            if(state_save_load_continue())
                return false;

            cpuintrf_pop_context();
        }

        /* finish and close */
        state_save_load_finish();

        
        return true;
	}

	return false;
}

static float              delta_samples;
int                       samples_per_frame = 0;
int                       orig_samples_per_frame =0;
short*                    samples_buffer;
short*                    conversion_buffer;
int                       usestereo = 1;

int osd_start_audio_stream(int stereo)
{
    if ( Machine->drv->frames_per_second * 1000 < options.samplerate)
      Machine->sample_rate=22050;

    else
      Machine->sample_rate = options.samplerate;
  
  delta_samples = 0.0f;
  usestereo = stereo ? 1 : 0;

  /* determine the number of samples per frame */
  samples_per_frame = Machine->sample_rate / Machine->drv->frames_per_second;
  orig_samples_per_frame = samples_per_frame;

  if (Machine->sample_rate == 0) return 0;

  samples_buffer = (short *) calloc(samples_per_frame+16, 2 + usestereo * 2);
  if (!usestereo) conversion_buffer = (short *) calloc(samples_per_frame+16, 4);
  
  return samples_per_frame;
}


int osd_update_audio_stream(INT16 *buffer)
{
	int i,j;
	if ( Machine->sample_rate !=0 && buffer )
	{
   		memcpy(samples_buffer, buffer, samples_per_frame * (usestereo ? 4 : 2));
		if (usestereo)
			audio_batch_cb(samples_buffer, samples_per_frame);
		else
		{
			for (i = 0, j = 0; i < samples_per_frame; i++)
        		{
				conversion_buffer[j++] = samples_buffer[i];
				conversion_buffer[j++] = samples_buffer[i];
		        }
         		audio_batch_cb(conversion_buffer,samples_per_frame);
		}	
		
			
		//process next frame
			
		if ( samples_per_frame  != orig_samples_per_frame ) samples_per_frame = orig_samples_per_frame;
		
		// dont drop any sample frames some games like mk will drift with time

		delta_samples += (Machine->sample_rate / Machine->drv->frames_per_second) - orig_samples_per_frame;
		if ( delta_samples >= 1.0f )
		{
		
			int integer_delta = (int)delta_samples;
			if (integer_delta <= 16 )
                        {
				log_cb(RETRO_LOG_DEBUG,"sound: Delta added value %d added to frame\n",integer_delta);
				samples_per_frame += integer_delta;
			}
			else if(integer_delta >= 16) log_cb(RETRO_LOG_INFO, "sound: Delta not added to samples_per_frame too large integer_delta:%d\n", integer_delta);
			else log_cb(RETRO_LOG_DEBUG,"sound(delta) no contitions met\n");	
			delta_samples -= integer_delta;

		}
	}
        return samples_per_frame;
}


void osd_stop_audio_stream(void)
{
}


// Stubs
unsigned retro_get_region (void) {return RETRO_REGION_NTSC;}
void *retro_get_memory_data(unsigned type) {return 0;}
size_t retro_get_memory_size(unsigned type) {return 0;}
bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info){return false;}
void retro_cheat_reset(void){}
void retro_cheat_set(unsigned unused, bool unused1, const char* unused2){}
void retro_set_controller_port_device(unsigned in_port, unsigned device){}
