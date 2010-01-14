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

jack_port_t * in;
jack_port_t * out;
jack_midi_data_t tab[128][3];
jack_midi_data_t ch[16];

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
        return -1; // not playable
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
    void * in_buffer  = jack_port_get_buffer(in, nframes);
    void * out_buffer = jack_port_get_buffer(out, nframes);
    if (in_buffer == NULL || out_buffer == NULL) {
        puts("Error: Couldn't get buffer!");
    }
    jack_midi_clear_buffer(out_buffer);

    jack_nframes_t i;
    jack_nframes_t count = jack_midi_get_event_count(in_buffer);
    for (i=0; i != count; ++i) {
        if (jack_midi_event_get(&event, in_buffer, i)) {
            puts("Error: Coudln't receive MIDI event!");
        }
        /* printf("received event: time %d, ", event.time); */
        /* int j; */
        /* for (j=0; j != event.size; ++j) { */
        /*     printf("%x ", event.buffer[j]); */
        /* } */
        /* printf("\n"); */
        if ((event.buffer[0] & 0xf0) == NOTEON) {
            int k = event.buffer[1];
            if (tab[k][0] == 0xff) { 
                /* printf("Note on: key %x is unmapped!\n", k); */
            } else {
                /* printf("Note on: key %x maps to %2x, pitch %2x %2x\n",  */
                /*        k, tab[k][0], tab[k][1], tab[k][2]); */
                jack_midi_data_t c;
                for (c=0; c < 16; ++c) {
                    if (ch[c] != 0xff) // channel occupied
                        continue;
                    ch[c] = tab[k][0];
                    // pitch bend
                    temp[0] = PITCHBEND + c; temp[1] = tab[k][1]; temp[2] = tab[k][2];
                    jack_midi_event_write(out_buffer, event.time, temp, event.size);
                    // note on message
                    temp[0] = NOTEON + c; temp[1] = tab[k][0]; temp[2] = event.buffer[2];
                    jack_midi_event_write(out_buffer, event.time, temp, event.size);
                    break;
                }
            }
        } else if ((event.buffer[0] & 0xf0) == NOTEOFF) {
            int k = event.buffer[1];
            if (tab[k][0] == 0xff) { 
                /* printf("Note off: key %x is unmapped!\n", k); */
            } else {
                /* printf("Note off: key %x maps to %2x, pitch %2x %2x\n",  */
                /*        k, tab[k][0], tab[k][1], tab[k][2]); */
                jack_midi_data_t c;
                for (c=0; c < 16; ++c) {
                    if (ch[c] != tab[k][0]) // wrong channel
                        continue;
                    ch[c] = 0xff;
                    // note off message
                    temp[0] = NOTEOFF + c; temp[1] = tab[k][0]; temp[2] = event.buffer[2];
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
    // default values for keyboard mapping
    // see: http://www.huygens-fokker.org/scala/help.htm#mappings
    int    center_key    = 69;    // this note is tuned directly,
    double center_freq   = 440.0; //   with this frequency.
    int    map_size      = 12;    // keyboard mapping repeats every x keys.
    int    first_note    = 0;     // first note to be mapped
    int    middle_note   = 60;    // keyboard mapping starts here
    int    last_note     = 127;   // last note to be mapped
    int    formal_octave = 12;    // this many keys form one formal octave
                                  //   (should be the same as scale length?)

    // default pitch bend range value is +/- 200 cents
    double pitch_range = 200.0;

    FILE * scale_file = NULL; // .scl
    FILE * key_file   = NULL; // .kbm (optional)

    int map[128]; // keyboard mapping (one formal octave)
    int m[128];   // global keyboard mapping

    int key[128]; // key translation table

    int    scale_length; // number of steps in scale
    double scale[128];   // scale steps in cents (one formal octave)

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
            if ((key_file = fopen(optarg, "r")) == NULL) {
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
    if ((scale_file = fopen(argv[optind], "r")) == NULL) {
        printf("Error: Scale file not found!\n");
        return usage();
    }

    // read scale file
    do {
        char line[BUFFERSIZE];

        do { fgets(line, BUFFERSIZE, scale_file); } while (line[0] == '!') ;
        printf("Reading scale file:\n%s", line);

        do { fgets(line, BUFFERSIZE, scale_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &scale_length);

        do { fgets(line, BUFFERSIZE, scale_file); } while (line[0] == '!') ;
        int i;
        for (i = 0; i != scale_length; ++i) {
            scale[i] = pitch2cent(line);
            fgets(line, BUFFERSIZE, scale_file);
        }
        
        fclose(scale_file);
    } while (0);    

    // read keyboard mapping
    if (key_file != NULL) {
        char line[BUFFERSIZE];

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &map_size);

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &first_note);

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &last_note);

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &middle_note);

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &center_key);

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        sscanf(line, "%lf", &center_freq);

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        sscanf(line, "%d", &formal_octave);

        do { fgets(line, BUFFERSIZE, key_file); } while (line[0] == '!') ;
        int i;
        for (i = 0; i != map_size; ++i) {
            if (line[0] == 'x')
                map[i] = -1;
            else
                sscanf(line, "%d", &(map[i]));
            fgets(line, BUFFERSIZE, key_file);
        }
    } else { // no kbm given, writing standard one
        int i;
        for (i = 0; i != map_size; ++i) {
            map[i] = i;
        }
    }

    // fill keyrange with mapped keys
    m[middle_note] = middle_note;
    int i;
    for (i = middle_note + 1; i < 128; ++i) {
        if (i > last_note) {
            m[i] = -1;
            continue;
        }
        int n_maps = (i - middle_note) / map_size;
        m[i] = middle_note + n_maps*formal_octave;
        int n_key = (i - middle_note) % map_size;
        if (map[n_key] == -1) {
            m[i] = -1;
        } else {
            m[i] += map[n_key];
        }
    }
    for (i = 0; i < middle_note; ++i) {
        if (i < first_note) {
            m[i] = -1;
            continue;
        }
        int n_maps = (i - middle_note) / map_size - 1;
        m[i] = middle_note + n_maps*formal_octave;
        int n_key = (i + 128*map_size - middle_note) % map_size;
        if (map[n_key] == -1) {
            m[i] = -1;
        } else {
            if (n_key == 0)
                m[i] += formal_octave;
            m[i] += map[n_key];
        }
    }

    // fill whole keyrange with freq values
    double freq[128];
    freq[center_key] = center_freq;
    for (i=center_key + 1; i < 128; ++i) {
        // first move to right proto-octave
        int n_octave = (i - center_key) / scale_length;
        freq[i] = center_freq * pow(2.0, scale[scale_length - 1]*n_octave/1200.0);
        // next ajust key within proto-octave
        int n_key = (i - center_key) % scale_length;
        if (n_key > 0)
            freq[i] *= pow(2.0, scale[n_key - 1]/1200.0);
    }
    for (i=0; i < center_key; ++i) {
        // first move to right proto-octave
        int n_octave = (i - center_key) / scale_length - 1;
        freq[i] = center_freq * pow(2.0, scale[scale_length - 1]*n_octave/1200.0);
        // next ajust key within proto-octave
        int n_key = (i + 128*scale_length - center_key) % scale_length;
        if (n_key == 0) {
            freq[i] *= pow(2.0, scale[scale_length - 1]/1200.0);
        } else if (n_key > 0) {
            freq[i] *= pow(2.0, scale[n_key - 1]/1200.0);
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
            tab[i][0] = 0xff;
            tab[i][1] = 0xff;            
            tab[i][2] = 0xff;
            continue;
        } else {
            tab[i][0] = key[m[i]];
            int p = pitch[m[i]];
            // convert int to two 7bit bytes (< 16384)
            // LSB first?
            tab[i][2] = (p & 0x3f80) >> 7;
            tab[i][1] = (p & 0x007f);            
        }
    }

    printf("Resulting translation table:\n");
    printf("key map   12tet freq   target freq  key +  +   pitch\n");
    for (i=0; i < 128; ++i) {
        if (m[i] == -1) {
            printf("%3x --- %12f    ---         ---   ---\n", i, old_freq[i]);
        } else if (key[i] != -1) {
            printf("%3x %3x %12f  %12f  %2x %2x %2x %6d\n",
                   i, m[i], old_freq[i], freq[i], tab[i][0], tab[i][1], tab[i][2], pitch[i]);
        } else {
            printf("%3x %3x %12f  %12f  ---   ---\n", i, m[i], old_freq[i], freq[i]);
        }
    }

    // clear midi channels
    for (i=0; i < 16; ++i) {
        ch[i] = 0xff; // this key is never mapped to
    }

    // open jack client
    jack_client_t * client = jack_client_open("fishnpitch", JackNoStartServer, NULL);
    if (client == NULL) {
        printf("Error: cannot open JACK client!\n");
        return 1;
    }

    jack_set_process_callback(client, process, 0);

    in = jack_port_register(client, "in", JACK_DEFAULT_MIDI_TYPE, JackPortIsInput, 0);
    out = jack_port_register(client, "out", JACK_DEFAULT_MIDI_TYPE, JackPortIsOutput, 0);

    if (jack_activate(client)) {
        printf("Error: cannot activate JACK client");
        return 1;
    }

    while(1) {
        sleep(10);
    }

    jack_client_close (client);
    exit (0);
}
