#include <math.h>
#include <stdio.h>
//#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <jack/jack.h>
#include <jack/midiport.h>

#define BUFFERSIZE 256
#define NOTEON     0x90
#define NOTEOFF    0x80

jack_port_t * in;
jack_port_t * out;
jack_midi_data_t tab[128][3];
jack_midi_data_t ch[16];

int usage() {
    //printf("fishnpitch - JACK MIDI realtime tuner \n");
    printf("Usage: fishnpitch -s SCALE [-k KEYBOARD_MAPPING]\n");
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
    void * in_buffer = jack_port_get_buffer(in, nframes);
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
                    temp[0] = 0xe0 + c; temp[1] = tab[k][1]; temp[2] = tab[k][2];
                    jack_midi_event_write(out_buffer, event.time, temp, event.size);
                    // note on message
                    temp[0] = 0x90 + c; temp[1] = tab[k][0]; temp[2] = event.buffer[2];
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
                    temp[0] = 0x80 + c; temp[1] = tab[k][0]; temp[2] = event.buffer[2];
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
    if (argc != 3 && argc != 5) {
        printf("Error: wrong number of arguments!\n");
        return usage();
    }

    FILE * scale_file;
    FILE * key_file;
    int scale_length;
    double scale[128];
    double freq[128];
    int center_key = 69;
    double center_freq = 440.0;
    int map_size = 12;
    int first_note = 0;
    int last_note = 127;
    int middle_note = 60;
    int formal_octave = 12;
    int map[128];
    int key[128];
    int m[128];

    // read scale, key files
    int i;
    char scale_read = 0;
    for(i = 1; i < argc; i += 2) {
        if (strcmp("-s", argv[i]) == 0) {
            if ((scale_file = fopen(argv[i + 1], "r")) == NULL) {
                printf("Error: scale file not found!\n");
                return 1;
            }
            char line[BUFFERSIZE];
            char b_descr = 1;
            char b_len = 1;
            int j = 0;
            while(fgets(line, BUFFERSIZE, scale_file)) {
                if (line[0] == '!')
                    continue;
                if (b_descr) {
                    printf("Reading scale file:\n%s", line);
                    b_descr = 0;
                }
                else if (b_len) {
                    sscanf(line, "%d", &scale_length);
                    b_len = 0;
                } else {
                    scale[j++] = pitch2cent(line);
                }
            }
            fclose(scale_file);
            scale_read = 1;
        } else if (strcmp("-k", argv[i]) == 0) {
            if ((key_file = fopen(argv[i + 1], "r")) == NULL) {
                printf("Error: key file not found!\n");
                return 1;
            }
            char line[BUFFERSIZE];
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            sscanf(line, "%d", &map_size);
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            sscanf(line, "%d", &first_note);
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            sscanf(line, "%d", &last_note);
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            sscanf(line, "%d", &middle_note);
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            sscanf(line, "%d", &center_key);
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            sscanf(line, "%lf", &center_freq);
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            sscanf(line, "%d", &formal_octave);
            fgets(line, BUFFERSIZE, key_file);
            while(line[0] == '!')
                fgets(line, BUFFERSIZE, key_file);
            int j;
            for (j=0; j < map_size; ++j) {
                if (line[0] == 'x')
                    map[j] = -1;
                else
                    sscanf(line, "%d", &(map[j]));
                fgets(line, BUFFERSIZE, key_file);
            }
        } else {
            printf("Error: wrong arguments!\n");
            return usage();
        }
    }
    
    if (scale_read) {
        /* printf("Scale: \n"); */
        /* for (i = 0; i < scale_length; ++i) { */
        /*     printf("%3d %15f\n", i, scale[i]); */
        /* } */
    } else {
        printf("Error: Did not read scale!\n");
        return usage();
    }

    if (argc == 5) { // using kbm
        /* printf("Keyboard mapping:\n"); */
        /* for (i=0; i < map_size; ++i) { */
        /*     if (map[i] == -1) { */
        /*         printf("%3d ---\n", i); */
        /*     } else { */
        /*         printf("%3d %3d\n", i, map[i]); */
        /*     } */
        /* } */
    } else { // no kbm given, writing standard one
        for (i=0; i < map_size; ++i) {
            map[i] = i;
        }
    }

    // fill keyrange with mapped keys
    m[middle_note] = middle_note;
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
    // pitch range is +/- 200 cents
    //int pitch_low = 0;
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

    /* printf("Resulting translation table:\n"); */
    /* printf("key map   12tet freq   target freq  key +  +   pitch\n"); */
    /* for (i=0; i < 128; ++i) { */
    /*     if (m[i] == -1) { */
    /*         printf("%3x --- %12f    ---         ---   ---\n", i, old_freq[i]); */
    /*     } else if (key[i] != -1) { */
    /*         printf("%3x %3x %12f  %12f  %2x %2x %2x %6d\n", */
    /*                i, m[i], old_freq[i], freq[i], tab[i][0], tab[i][1], tab[i][2], pitch[i]); */
    /*     } else { */
    /*         printf("%3x %3x %12f  %12f  ---   ---\n", i, m[i], old_freq[i], freq[i]); */
    /*     } */
    /* } */

    // clear midi channels
    for (i=0; i < 16; ++i) {
        ch[i] = 0xff; // this key is never mapped to
    }

    // open jack client
    jack_status_t * status;
    jack_client_t * client = jack_client_open("fishnpitch", JackNoStartServer, status);
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
