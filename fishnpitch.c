//
// Copyright 2010, Robert Schwarz <mail@rschwarz.net>
//
// fishnpitch - a JACK MIDI realtime tuner
//  
//   This program is free software: you can redistribute it and/or modify
//   it under the terms of the GNU General Public License as published by
//   the Free Software Foundation, either version 3 of the License, or
//   (at your option) any later version.
//
//   This program is distributed in the hope that it will be useful,
//   but WITHOUT ANY WARRANTY; without even the implied warranty of
//   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//   GNU General Public License for more details.
//
//   You should have received a copy of the GNU General Public License
//   along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#define BUFFERSIZE  256
#define NOTEON     0x90
#define NOTEOFF    0x80
#define PITCHBEND  0xe0

// global variables
jack_port_t *    g_in;          // JACK MIDI input
jack_port_t *    g_out;         // JACK MIDI output
jack_midi_data_t g_tab[128][3]; // MIDI data for translation
jack_midi_data_t g_ch[16];      // current note for each channel
jack_midi_data_t g_next[16];    // next free channel in queue
jack_midi_data_t g_first;       // first free channel in queue
jack_midi_data_t g_last;        // last free channel in queue
                                //   Even after a note off event, some 
                                //   sound is heard, so we shouldn't
                                //   change pitch. Better use the oldest
                                //   note first.

int usage() {
    printf("Usage: fishnpitch [OPTIONS] SCALE \n");
    printf("Options:\n");
    printf("  -k KEYBOARDMAPPING \ta .scl file\n");
    printf("     default keyboard mapping uses all 12 keys\n");
    printf("     and tunes key 69 to 440.0 Hz\n");
    printf("  -p PITCH_RANGE     \tin cents\n"); 
    printf("     default pitch bend range is +/- 200.0 cents (2 semitones)\n");
    printf("  -c CHANNELS        \te.g. 0123456789abcdef\n");
    printf("     by default use all 16 channels, write 0 to use just the first\n");
    printf("     number of channels bounds polyphony\n");
    return 1;
}

double pitch2cent(char * line) {
    double result = 0.0;
    int n = 1;
    int d = 1;
    if (strchr(line, '.') != NULL) {
        sscanf(line, "%lf", &result);
    } else if (strchr(line, '/') != NULL) {
        sscanf(line, "%d/%d", &n, &d);
        result = (double) n*1.0/d;
        result = 1200.0 / log(2.0) * log(result);
    } else {
        sscanf(line, "%d", &n);
        result = 1200.0 / log(2.0) * log(n);
    }
    return result;
}

int find_key(double freq, double * old_freq) {
    if (old_freq[0] > freq || old_freq[127] < freq) {
        return -1; // not playable (simplification)
    }

    // binary search
    int left = 0;
    int right = 127;
    int middle = 0;
    while (right - left > 1) {
        middle = (left + right)/2 ;
        if (freq <= old_freq[middle]) {
            right = middle;
        } else {
            left = middle;
        }
    }
    return left;
}

int process(jack_nframes_t nframes, void * arg) {
    /* puts("---- process ----"); */
    /* jack_midi_data_t c; */
    /* for (c=0; c < 16; ++c) { */
    /*     printf("%x ", ch[c]); */
    /* } */
    /* printf("\n"); */
    jack_midi_event_t event;
    jack_midi_data_t temp[3];
    void * in_buffer  = jack_port_get_buffer(g_in, nframes);
    void * out_buffer = jack_port_get_buffer(g_out, nframes);
    if (in_buffer == NULL || out_buffer == NULL) {
        puts("Error: Couldn't get buffer!");
    }
    jack_midi_clear_buffer(out_buffer);

    jack_nframes_t i;
    jack_nframes_t count = jack_midi_get_event_count(in_buffer);
    for (i = 0; i != count; ++i) {
        if (jack_midi_event_get(&event, in_buffer, i)) {
            puts("Error: Couldn't receive MIDI event!");
        }
        /* printf("received event: time %d, ", event.time); */
        /* int j; */
        /* for (j=0; j != event.size; ++j) { */
        /*     printf("%x ", event.buffer[j]); */
        /* } */
        /* printf("\n"); */
        if ((event.buffer[0] & 0xf0) == NOTEON) {
            jack_midi_data_t k = event.buffer[1];
            if (g_tab[k][0] == 0xff) { 
                /* printf("Note on: key %x is unmapped!\n", k); */
            } else {
                /* printf("Note on: key %x maps to %2x, pitch %2x %2x\n",  */
                /*        k, tab[k][0], tab[k][1], tab[k][2]); */
                jack_midi_data_t c = g_first;
                if (c == 0xff) {
                    // channel occupied, no playback possible
                    continue;
                }
                // occupy channel
                g_ch[c] = g_tab[k][0]; // set channel's current note
                g_first = g_next[c];   // move first to next free channel
                g_next[c] = 0xff;      // is no longer in queue
                if (g_last == c)       // if this was last free channel
                    g_last = 0xff;     //   say so, for later appending

                // pitch bend message
                temp[0] = PITCHBEND + c; temp[1] = g_tab[k][1]; temp[2] = g_tab[k][2];
                jack_midi_event_write(out_buffer, event.time, temp, event.size);
                // note on message
                temp[0] = NOTEON + c; temp[1] = g_tab[k][0]; temp[2] = event.buffer[2];
                jack_midi_event_write(out_buffer, event.time, temp, event.size);
            }
        } else if ((event.buffer[0] & 0xf0) == NOTEOFF) {
            jack_midi_data_t k = event.buffer[1];
            if (g_tab[k][0] == 0xff) { 
                /* printf("Note off: key %x is unmapped!\n", k); */
            } else {
                /* printf("Note off: key %x maps to %2x, pitch %2x %2x\n",  */
                /*        k, tab[k][0], tab[k][1], tab[k][2]); */
                jack_midi_data_t c;
                for (c = 0; c != 16; ++c) {
                    if (g_ch[c] != g_tab[k][0]) // unique channel for every note?!
                        continue;
                    // make channel available again
                    g_ch[c] = 0xff;         // isn't occupied anymore
                    if (g_last == 0xff) {   // if queue is empty at the moment
                        g_first = c;        //   both ends point to this channel
                        g_last = c;         //
                    } else {                // append at queue's end
                        g_next[g_last] = c; //   connect to one before last
                        g_last = c;         //   and point end to c
                    }
                    // note off message
                    temp[0] = NOTEOFF + c; temp[1] = g_tab[k][0]; temp[2] = event.buffer[2];
                    jack_midi_event_write(out_buffer, event.time, temp, event.size);
                    break;
                }
            }
        } else { // directly forward midi event
            jack_midi_event_write(out_buffer, event.time, event.buffer, event.size);
        }
    }
    return 0;
}

int main(int argc, char *argv[]) {
    // scale
    //   see: http://www.huygens-fokker.org/scala/scl_format.html
    FILE * scl_file = NULL;                          // .scl
    int    scl_length;                               // number of deg in scale
    double * scl_deg = malloc(128 * sizeof(double)); // scale deg in cents (one formal octave)

    // keyboard mapping, with default values
    //   see: http://www.huygens-fokker.org/scala/help.htm#mappings
    FILE * kbm_file           = NULL;  // .kbm (optional)
    int    kbm_size           = 12;    // keyboard mapping repeats every x keys.
    int    kbm_first_note     = 0;     // first note to be mapped
    int    kbm_middle_note    = 60;    // keyboard mapping starts here
    int    kbm_last_note      = 127;   // last note to be mapped
    int    kbm_ref_note       = 69;    // this note is tuned directly,
    double kbm_ref_freq       = 440.0; //   with this frequency.
    int    kbm_form_oct       = 12;    // this many keys form one formal octave

    int *  kbm_deg = malloc(128 * sizeof(int)); // keyboard mapping (one formal octave)

    int pitch_range  = 200;   // default pitch bend range value is +/- 200 cents
    int pitch_middle = 8192;  //  no pitch bend
    int pitch_high   = 16384; // max pitch bind

    int i;

    for (i = 0; i != 16; ++i) {
        g_ch[i] = 1;
    }

    // process arguments
    int c = 0;
    opterr = 0;
    while ((c = getopt (argc, argv, "k:p:c:")) != -1) {
        switch (c) {
        case 'p':
            sscanf(optarg, "%d", &pitch_range);
            printf("Setting pitch range: %d\n", pitch_range);
            break;
        case 'k':
            if ((kbm_file = fopen(optarg, "r")) == NULL) {
                printf("Error: Keyboard mapping file not found!\n");
                return usage();
            }
            break;
        case 'c':
            for (i = 0; i != 16; ++i) {
                char hex[] = "0123456789abcdef";
                if (strchr(optarg, hex[i]) == NULL)
                    g_ch[i] = 0; // deactivate channel
            }
            break;
        case '?':
            if (optopt == 'k' || optopt == 'p')
                printf("Error: Option -%c requires an argument.\n", optopt);
            else if (isprint(optopt))
                printf("Error: Unknown option `-%c'.\n", optopt);
            else
                printf("Error: Unknown option character `\\x%x'.\n", optopt);
            return usage();
        default:
            abort();
        }
    }
    if (argc - optind > 1) {
        printf("Error: Too many non-option arguments!\n");
        return usage();
    } else if (argc - optind < 1) {
        printf("Error: No scale file given!\n");
        return usage();        
    }
    if ((scl_file = fopen(argv[optind], "r")) == NULL) {
        printf("Error: Scale file not found!\n");
        return usage();
    }

    // read scale file
    do {
        char * line = malloc(BUFFERSIZE * sizeof(char));

        do { fgets(line, BUFFERSIZE, scl_file); } while (line[0] == '!') ;
        printf("Reading scale file:\n%s", line);

        do { fgets(line, BUFFERSIZE, scl_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &scl_length);

        do { fgets(line, BUFFERSIZE, scl_file); } while (line[0] == '!') ;
        for (i = 0; i != scl_length; ++i) {
            scl_deg[i] = pitch2cent(line);
            fgets(line, BUFFERSIZE, scl_file);
            /* printf("%d %f\n", i, scl_deg[i]); */
        }
        
        free(line);
        fclose(scl_file);
    } while (0);

    // read keyboard mapping
    if (kbm_file != NULL) {
        char * line = malloc(BUFFERSIZE * sizeof(char));

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &kbm_size);

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &kbm_first_note);

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &kbm_last_note);

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &kbm_middle_note);

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &kbm_ref_note);

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        sscanf(line, "%lf", &kbm_ref_freq);

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &kbm_form_oct);

        do { fgets(line, BUFFERSIZE, kbm_file); } while (line[0] == '!') ;
        for (i = 0; i != kbm_size; ++i) {
            if (line[0] == 'x')
                kbm_deg[i] = -1;
            else
                sscanf(line, "%d", &(kbm_deg[i]));
            fgets(line, BUFFERSIZE, kbm_file);
            /* printf("%d %d\n", i, kbm_deg[i]); */
        }

        free(line);
    } else { // no kbm given, writing standard one
        kbm_size           = scl_length;
        kbm_form_oct       = scl_length;
        for (i = 0; i != kbm_size; ++i) {
            kbm_deg[i] = i;
        }
    }

    // compute target frequency values
    double * target_freq = malloc(128 * sizeof(double));
    double ref_freq; // first compute the frequency for kbm_middle_note
    do {
        int i = kbm_ref_note;
        int q = (i - kbm_middle_note) / kbm_size;
        int r = (128*kbm_size + i - kbm_middle_note) % kbm_size;
        if (i < kbm_middle_note && r != 0) // strange rounding behaviour
            q -= 1;
        if (r == 0) {
            ref_freq = kbm_ref_freq / pow(2.0, (scl_deg[scl_length - 1]*q)/1200.0);
        } else {
            ref_freq = kbm_ref_freq / pow(2.0, (scl_deg[scl_length - 1]*q + scl_deg[r - 1])/1200.0);
        }
        /* printf("Ref.key %3d  %3d  %15f\n", q, r, ref_freq); */
    } while (0);
    for(i = 0; i != 128; ++i) {
        int q = (i - kbm_middle_note) / kbm_size;
        int r = (128*kbm_size + i - kbm_middle_note) % kbm_size;
        if (i < kbm_middle_note && r != 0) // strange rounding behaviour
            q -= 1;
        int mk; // mapped key
        if (kbm_deg[r] == -1) { // not mapped
            mk = -1;
        } else {
            mk = kbm_middle_note + q * kbm_form_oct + kbm_deg[r];
        }
        if (mk < 0 || mk > 127) { // out of range (or not mapped)
            target_freq[i] = -1.0;
            /* printf("%3d  %3d  %3d  %3d  ---  ---      ---\n", i, q, r, mk); */
        } else {
            int qq = (mk - kbm_middle_note) / scl_length;
            int rr = (128*kbm_size + mk - kbm_middle_note) % scl_length;
            if (mk < kbm_middle_note && rr != 0) // strange rounding behaviour
                qq -= 1;
            if (rr == 0) {
                target_freq[i] = ref_freq * pow(2.0, scl_deg[scl_length - 1]*qq/1200.0);
            } else {
                target_freq[i] = ref_freq * pow(2.0, (scl_deg[scl_length - 1]*qq + scl_deg[rr - 1])/1200.0);
            }
            /* printf("%3d  %3d  %3d  %3d  %3d  %3d  %14f\n", i, q, r, mk, qq, rr, target_freq[i]); */
        }
    }

    // compute standard 12tet frequency values
    double * source_freq = malloc(128 * sizeof(double));
    for (i = 0; i != 128; ++i) {
        source_freq[i] = 440.0 * pow(2.0, -5 - 900./1200. + i*100.0/1200.);
    }
    
    // write translation table for midi data
    for (i = 0; i != 128; ++i) {
        int new_key = find_key(target_freq[i], source_freq);
        if (new_key == -1) { // key is not mappable
            g_tab[i][0] = 0xff;
            g_tab[i][1] = 0xff;
            g_tab[i][2] = 0xff;            
            continue;
        }

        g_tab[i][0] = new_key;
        double ratio = target_freq[i] / source_freq[new_key];
        double cents = log(ratio) * 1200.0 / log(2.0);
        int    pitch = pitch_middle + (pitch_high - pitch_middle) * cents / pitch_range;
        // convert int to two 7bit bytes (<= 16384), LSB first?
        g_tab[i][2] = (pitch & 0x3f80) >> 7;
        g_tab[i][1] = (pitch & 0x007f);
    }

    // prepare midi channels
    g_first = 0xff;
    g_last  = 0xff;
    for (i = 0; i < 16; ++i) {
        if (g_ch[i] == 0) // deactivated channel
            continue;
        if (g_first == 0xff)
            g_first = i;  // will be on first activated
        g_last = i;       // will be on last activated
        g_next[i] = 0xff; // will all be 0xff
    }
    if (g_first == 0xff) {
        printf("Error: No MIDI channel is free!\n");
        return usage();
    }
    int cur = g_first;
    for (i = cur + 1; i <= g_last; ++i) {
        if (g_ch[i] == 0) // deactivated channel
            continue;
        g_next[cur] = i;
        cur = i;
    }
    for (i = 0; i < 16; ++i) {
        g_ch[i]   = 0xff; 
    }

    // free temporary memory
    free(kbm_deg);
    free(target_freq);
    free(source_freq);
    free(scl_deg);

    // open jack client
    jack_client_t * client = jack_client_open("fishnpitch", JackNoStartServer, NULL);
    if (client == NULL) {
        printf("Error: cannot open JACK client!\n");
        return 1;
    }

    jack_set_process_callback(client, process, 0);
    g_in  = jack_port_register(client, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    g_out = jack_port_register(client, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    if (jack_activate(client)) {
        printf("Error: cannot activate JACK client!\n");
        return 1;
    }

    while(1) {
        sleep(10);
    }

    jack_client_close(client);
    exit(0);
}
