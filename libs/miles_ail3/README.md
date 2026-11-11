# miles_ail3 - Miles Sound System (AIL 3.x) at the API level

Generic: replaces AIL_API_* digital (samples, double-buffered streams) and XMIDI functions with an
SDL mixer and an XMIDI player on the Windows MIDI synth. AIL's timer services stay recompiled.
1. `python tools/ail_api.py gamedata/cd/GAME.EXE` - name -> AIL_API implementation address (from the
   wrappers' debug format strings). 2. `python tools/gen_ail_natives.py` writes them into every
   tools/native_<tag>.txt below the "# --- AIL" marker. 3. Rebuild. game_program_start calls sound_reset.
Some games call AIL_API functions this file does not implement yet - the build fails with an
undefined N_AIL_* reference: add it to native_ail.c (or map it to N_AIL_nop / ret0 / ret1).
