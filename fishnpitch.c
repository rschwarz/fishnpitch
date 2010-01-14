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
jack_port_t *    g_in;
jack_port_t *    g_out;
jack_midi_data_t g_tab[128][3];
jack_midi_data_t g_ch[16];

int usage() {
    printf("Usage: fishnpitch [-k KEYBOARD_MAPPING] [-p PITCH_RANGE (cents)] SCALE \n");
    printf("  default keymapping uses all 12 keys and tunes key 69 to 440.0 Hz\n");
    printf("  default pitch bend range is +/- 200.0 cents (2 semitones)");
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
                jack_midi_data_t c;
                for (c = 0; c != 16; ++c) {
                    if (g_ch[c] != 0xff) // channel occupied
                        continue;
                    g_ch[c] = g_tab[k][0];
                    // pitch bend message
                    temp[0] = PITCHBEND + c; temp[1] = g_tab[k][1]; temp[2] = g_tab[k][2];
                    jack_midi_event_write(out_buffer, event.time, temp, event.size);
                    // note on message
                    temp[0] = NOTEON + c; temp[1] = g_tab[k][0]; temp[2] = event.buffer[2];
                    jack_midi_event_write(out_buffer, event.time, temp, event.size);
                    break;
                }
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
                    if (g_ch[c] != g_tab[k][0]) // wrong channel
                        continue;
                    g_ch[c] = 0xff;
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
    FILE * scl_file = NULL; // .scl
    int    scl_length;      // number of deg in scale
    double scl_deg[128];        // scale deg in cents (one formal octave)

    // keyboard mapping, with default values
    //   see: http://www.huygens-fokker.org/scala/help.htm#mappings
    FILE * kbm_file          = NULL;  // .kbm (optional)
    int    kbm_size          = 12;    // keyboard mapping repeats every x keys.
    int    kbm_first_note    = 0;     // first note to be mapped
    int    kbm_middle_note   = 60;    // keyboard mapping starts here
    int    kbm_last_note     = 127;   // last note to be mapped
    int    kbm_ref_note      = 69;    // this note is tuned directly,
    double kbm_ref_freq      = 440.0; //   with this frequency.
    int    kbm_form_oct      = 12;    // this many keys form one formal octave
                                      //   (should be the same as scale length?)
    int    kbm_deg[128]; // keyboard mapping (one formal octave)

    // default pitch bend range value is +/- 200 cents
    double pitch_range = 200.0;


    int m[128];   // global keyboard mapping

    int key[128]; // key translation table

    // process arguments
    int c = 0;
    opterr = 0;
    while ((c = getopt (argc, argv, "k:p:")) != -1) {
        switch (c) {
        case 'p':
            sscanf(optarg, "%lf", &pitch_range);
            printf("Setting pitch range: %f\n", pitch_range);
            break;
        case 'k':
            if ((kbm_file = fopen(optarg, "r")) == NULL) {
                printf("Error: Keyboard mapping file not found!\n");
                return usage();
            }
            printf("Reading keyboard mapping: %s\n", optarg);
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
        char line[BUFFERSIZE];

        do { fgets(line, BUFFERSIZE, scl_file); } while (line[0] == '!') ;
        printf("Reading scale file:\n%s", line);

        do { fgets(line, BUFFERSIZE, scl_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &scl_length);

        do { fgets(line, BUFFERSIZE, scl_file); } while (line[0] == '!') ;
        int i;
        for (i = 0; i != scl_length; ++i) {
            scl_deg[i] = pitch2cent(line);
            fgets(line, BUFFERSIZE, scl_file);
        }
        
        fclose(scl_file);
    } while (0);    

    // read keyboard mapping
    if (kbm_file != NULL) {
        char line[BUFFERSIZE];

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
        int i;
        for (i = 0; i != kbm_size; ++i) {
            if (line[0] == 'x')
                kbm_deg[i] = -1;
            else
                sscanf(line, "%d", &(kbm_deg[i]));
            fgets(line, BUFFERSIZE, kbm_file);
        }
    } else { // no kbm given, writing standard one
        int i;
        for (i = 0; i != kbm_size; ++i) {
            kbm_deg[i] = i;
        }
    }

    // fill keyrange with mapped keys
    m[kbm_middle_note] = kbm_middle_note;
    int i;
    for (i = kbm_middle_note + 1; i < 128; ++i) {
        if (i > kbm_last_note) {
            m[i] = -1;
            continue;
        }
        int n_maps = (i - kbm_middle_note) / kbm_size;
        m[i] = kbm_middle_note + n_maps*kbm_form_oct;
        int n_key = (i - kbm_middle_note) % kbm_size;
        if (kbm_deg[n_key] == -1) {
            m[i] = -1;
        } else {
            m[i] += kbm_deg[n_key];
        }
    }
    for (i = 0; i < kbm_middle_note; ++i) {
        if (i < kbm_first_note) {
            m[i] = -1;
            continue;
        }
        int n_maps = (i - kbm_middle_note) / kbm_size - 1;
        m[i] = kbm_middle_note + n_maps*kbm_form_oct;
        int n_key = (i + 128*kbm_size - kbm_middle_note) % kbm_size;
        if (kbm_deg[n_key] == -1) {
            m[i] = -1;
        } else {
            if (n_key == 0)
                m[i] += kbm_form_oct;
            m[i] += kbm_deg[n_key];
        }
    }

    // fill whole keyrange with freq values
    double freq[128];
    freq[kbm_ref_note] = kbm_ref_freq;
    for (i=kbm_ref_note + 1; i < 128; ++i) {
        // first move to right proto-octave
        int n_octave = (i - kbm_ref_note) / scl_length;
        freq[i] = kbm_ref_freq * pow(2.0, scl_deg[scl_length - 1]*n_octave/1200.0);
        // next ajust key within proto-octave
        int n_key = (i - kbm_ref_note) % scl_length;
        if (n_key > 0)
            freq[i] *= pow(2.0, scl_deg[n_key - 1]/1200.0);
    }
    for (i=0; i < kbm_ref_note; ++i) {
        // first move to right proto-octave
        int n_octave = (i - kbm_ref_note) / scl_length - 1;
        freq[i] = kbm_ref_freq * pow(2.0, scl_deg[scl_length - 1]*n_octave/1200.0);
        // next ajust key within proto-octave
        int n_key = (i + 128*scl_length - kbm_ref_note) % scl_length;
        if (n_key == 0) {
            freq[i] *= pow(2.0, scl_deg[scl_length - 1]/1200.0);
        } else if (n_key > 0) {
            freq[i] *= pow(2.0, scl_deg[n_key - 1]/1200.0);
        }
    }

    // fill up standard 12tet freq
    double old_freq[128];
    for (i=0; i < 128; ++i) {
        old_freq[i] = 440.0 * pow(2.0, -5 - 900./1200. + i*100.0/1200.);
    }

    // compute translation table
    for (i=0; i < 128; ++i) {
        key[i] = find_key(freq[i], old_freq);
    }

    // compute pitch bend values
    int pitch[128];
    int pitch_middle = 8192;
    int pitch_high = 16384;
    for (i=0; i < 128; ++i) {
        if (key[i] == -1) {
            pitch[i] = -1;
            continue;
        }
        pitch[i] = pitch_middle;
        double p_ratio = freq[i]/old_freq[key[i]];
        if (p_ratio > 1.00001) {
            double p_cents = log(p_ratio) * 1200.0 / log(2.0);
            pitch[i] += (pitch_high - pitch_middle)*p_cents/200;
        }
    }

    // actually writing midi compatible table
    for (i=0; i < 128; ++i) {
        if (m[i] == -1 || key[m[i]] == -1) {
            g_tab[i][0] = 0xff;
            g_tab[i][1] = 0xff;            
            g_tab[i][2] = 0xff;
            continue;
        } else {
            g_tab[i][0] = key[m[i]];
            int p = pitch[m[i]];
            // convert int to two 7bit bytes (< 16384)
            // LSB first?
            g_tab[i][2] = (p & 0x3f80) >> 7;
            g_tab[i][1] = (p & 0x007f);            
        }
    }

    printf("Resulting translation table:\n");
    printf("key map   12tet freq   target freq  key +  +   pitch\n");
    for (i=0; i < 128; ++i) {
        if (m[i] == -1) {
            printf("%3x --- %12f    ---         ---   ---\n", i, old_freq[i]);
        } else if (key[i] != -1) {
            printf("%3x %3x %12f  %12f  %2x %2x %2x %6d\n",
                   i, m[i], old_freq[i], freq[i], g_tab[i][0], g_tab[i][1], g_tab[i][2], pitch[i]);
        } else {
            printf("%3x %3x %12f  %12f  ---   ---\n", i, m[i], old_freq[i], freq[i]);
        }
    }

    // clear midi channels
    for (i=0; i < 16; ++i) {
        g_ch[i] = 0xff; // this key is never mapped to
    }

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
        printf("Error: cannot activate JACK client");
        return 1;
    }

    while(1) {
        sleep(10);
    }

    jack_client_close(client);
    exit(0);
}
