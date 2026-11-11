# hmi_sos / hmp_midi - HMI Sound Operating System

From Aliens CBA. native_sound.c replaces the 32-bit SOS 4 digital API (sosDIGIStartSample,
StopSample, SetSampleVolume, SetPanLocation, SetPitch, SetMasterVolume, SampleDone) and Cryo's
sound-system init; native_midi.c (libs/hmp_midi) plays HMP songs on winmm. Both keep a few
game-visible variables at Aliens' addresses (marked in the files) - replace them with yours, and
list the API addresses in tools/native_<tag>.txt (see libs/examples/native_aliens.txt).
