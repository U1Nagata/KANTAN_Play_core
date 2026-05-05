// SPDX-License-Identifier: LicenseRef-KANTAN-Music
// Thin browser ABI for the binary KANTAN Music API.

#include "KANTANMusic.h"

#ifdef __cplusplus
extern "C" {
#endif

unsigned char kantan_music_get_midi_note_number(
    int pitch,
    int degree,
    int key,
    int voicing,
    int modifier,
    int semitone_shift,
    int bass_degree,
    int bass_semitone_shift,
    int position,
    int minor_swap)
{
    KANTANMusic_GetMidiNoteNumberOptions options;
    KANTANMusic_GetMidiNoteNumber_SetDefaultOptions(&options);
    options.voicing = (KANTANMusic_Voicing)voicing;
    options.modifier = (KANTANMusic_Modifier)modifier;
    options.semitone_shift = semitone_shift;
    options.bass_degree = bass_degree;
    options.bass_semitone_shift = bass_semitone_shift;
    options.position = position;
    options.minor_swap = minor_swap != 0;

    return KANTANMusic_GetMidiNoteNumber(pitch, degree, key, &options);
}

#ifdef __cplusplus
}
#endif
