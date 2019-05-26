/**
 * This file is part of spectralizer
 * which is licensed under the GPL v2.0
 * See LICENSE or http://www.gnu.org/licenses
 * github.com/univrsal/spectralizer
 */

#include "audio_processor.hpp"
#include "../../source/visualizer_source.hpp"

/*
    Most processing is reused from cava
    https://github.com/karlstav/cava/blob/master/cava.c
 */

namespace audio
{
    /* EQ used to weigh frequencies */
    double smoothing_values[] = {0.8, 0.8, 1, 1, 0.8, 0.8, 1, 0.8, 0.8, 1, 1, 0.8, 1, 1, 0.8, 0.6, 0.6, 0.7, 0.8, 0.8,
                                 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8,
                                 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.8, 0.7, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6,
                                 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6, 0.6};

    audio_processor::audio_processor(source::config* cfg)
    {
        update(cfg);
    }

    audio_processor::~audio_processor()
    {
        clean_up();
    }

    void audio_processor::update(source::config* cfg)
    {
        clean_up();
        m_buf_size = cfg->buffer_size * cfg->buffer_size;
        m_samples = 2 * (m_buf_size / 2 + 1);
        m_channels = cfg->stereo ? 2 : 1;
        m_eq_dist = 64. / cfg->detail;
        m_sleep_counter = 0.f;

        /* Utility arrays (bzalloc zeros the memory)*/
        m_fall_off = static_cast<int*>(bzalloc(cfg->detail * sizeof(int) * 2));
        m_last_freqs = static_cast<int*>(bzalloc(cfg->detail * sizeof(int) * 2));
        m_last_freqsd = static_cast<int*>(bzalloc(cfg->detail * sizeof(int) * 2));
        m_freq_mem = static_cast<int*>(bzalloc(cfg->detail * sizeof(int) * 2));
        m_freq_both = static_cast<int*>(bzalloc(cfg->detail * sizeof(int) * 2));

        m_low_freq_cut = static_cast<int*>(bzalloc(cfg->detail * sizeof(int)));
        m_high_freq_cut = static_cast<int*>(bzalloc(cfg->detail * sizeof(int)));
        m_freq_l = static_cast<int*>(bzalloc(cfg->detail * sizeof(int)));
        m_freq_r = static_cast<int*>(bzalloc(cfg->detail * sizeof(int)));

        m_freq_peak = static_cast<float*>(bzalloc(cfg->detail * sizeof(float)));
        m_freq_weight = static_cast<double*>(bzalloc(cfg->detail * sizeof(double)));

        m_fftw_in_l = static_cast<double*>(bzalloc(m_samples * sizeof(double)));
        m_fftw_in_r = static_cast<double*>(bzalloc(m_samples * sizeof(double)));

        /* setup fftw values */
        m_fftw_out_l = static_cast<fftw_complex*>(bzalloc(m_samples * sizeof(fftw_complex)));
        m_fftw_out_r = static_cast<fftw_complex*>(bzalloc(m_samples * sizeof(fftw_complex)));

        m_fftw_plan_l = fftw_plan_dft_r2c_1d(m_buf_size, m_fftw_in_l, m_fftw_out_l, FFTW_MEASURE);
        m_fftw_plan_r = fftw_plan_dft_r2c_1d(m_buf_size, m_fftw_in_r, m_fftw_out_r, FFTW_MEASURE);

        /* Misc caluclations, that only have to be done once per updated settings */
        double frequency_constant, pot, fre, fc;
        int n, smooth_index;

        m_current_gravity = cfg->gravity * ((float) cfg->bar_height / 2160) * pow((60 / (float) cfg->fps), 2.5);
        frequency_constant = log10(cfg->freq_cutoff_low / ((float) cfg->freq_cutoff_high)) / (1.f / (cfg->detail + 1.f)
                - 1);

        /* Caculate cut-off frequencies & and weigh frequencies */
        for (n = 0; n < cfg->detail + 1; n++) {
            pot = frequency_constant * (-1);
            pot += (n + 1.f) / (cfg->detail + 1.f) * frequency_constant;
            fc = cfg->freq_cutoff_high * pow(10, pot);
            fre = fc / (cfg->sample_rate / 2);

            /* */
            m_low_freq_cut[n] = fre * (m_buf_size / 2) + 1;
            if (n > 0) {
                m_high_freq_cut[n - 1] = m_low_freq_cut[n] - 1;

                /* Adjust spectrum if exp function "clumps" (idk what that means) */
                if (m_low_freq_cut[n] <= m_low_freq_cut[n - 1])
                    m_low_freq_cut[n] = m_low_freq_cut[n - 1] + 1;
                m_high_freq_cut[n - 1] = m_low_freq_cut[n] - 1;
            }
            debug("[%i] LFC: %i, HFC: %i", n, m_low_freq_cut[n], m_high_freq_cut[n]);

            if (n < cfg->detail) {
                /* Weigh frequencies */
                /* Smooth index grabs a smoothing value out of the predefined array of values */
                smooth_index = UTIL_CLAMP(0, (int) floor(n * m_eq_dist), 64);
                m_freq_weight[n] = pow(fc, .85);
                m_freq_weight[n] *= (float) cfg->bar_height / pow(2, 28);
                m_freq_weight[n] *= smoothing_values[smooth_index];
            }

        }
        UNUSED_PARAMETER(nullptr);
    }

    void audio_processor::clean_up()
    {
        /* Free allocated memory */
        bfree(m_fftw_in_l);
        bfree(m_fftw_in_r);
        bfree(m_fftw_out_l);
        bfree(m_fftw_out_r);

        fftw_destroy_plan(m_fftw_plan_l);
        fftw_destroy_plan(m_fftw_plan_r);

        /* Free utility arrays */
        bfree(m_fall_off);
        bfree(m_last_freqs);
        bfree(m_last_freqsd);
        bfree(m_low_freq_cut);
        bfree(m_high_freq_cut);
        bfree(m_freq_mem);
        bfree(m_freq_peak);
        bfree(m_freq_weight);
        bfree(m_freq_l);
        bfree(m_freq_r);
        bfree(m_freq_both);

        /* Set to null for good measure */
        m_freq_both = nullptr;
        m_fall_off = nullptr;
        m_last_freqs = nullptr;
        m_last_freqsd = nullptr;
        m_low_freq_cut = nullptr;
        m_high_freq_cut = nullptr;
        m_freq_mem = nullptr;
        m_freq_peak = nullptr;
        m_freq_weight = nullptr;
        m_freq_l = nullptr;
        m_freq_r = nullptr;

        m_fftw_in_l = nullptr;
        m_fftw_in_r = nullptr;
        m_fftw_out_l = nullptr;
        m_fftw_out_r = nullptr;

        m_fftw_plan_l = nullptr;
        m_fftw_plan_r = nullptr;
    }

    void audio_processor::tick(float seconds, source::config* cfg)
    {
        /* Copy over current values for next tick() */
        memcpy(m_last_freqsd, m_freq_both, sizeof(int) * cfg->detail * 2);

        int i, o;
        /* Process collected audio */
        bool silence = true;
        for (i = 0; i < m_samples; i++) {
            if (i < m_buf_size && i < AUDIO_SIZE) {
                m_fftw_in_l[i] = m_audio_out_l[i];

                if (m_channels > 1)
                    m_fftw_in_r[i] = m_audio_out_r[i];
                if (silence && (m_fftw_in_l[i] > 0 || m_fftw_in_r[i] > 0))
                    silence = false;
            } else {
                m_fftw_in_l[i] = 0;
                if (m_channels > 1)
                    m_fftw_in_r[i] = 0;
            }
        }

        if (silence)
            m_sleep_counter += seconds;
        else
            m_sleep_counter = 0;

        if (m_sleep_counter < 5) { /* Audio for >5 seconds -> can process */
            m_can_draw = true;
            if (!log_flag) {
                debug("Got audio. Starting Visualization");
            }
            log_flag = true;
            if (m_channels > 1) {
                fftw_execute(m_fftw_plan_l);
                fftw_execute(m_fftw_plan_r);
                separate_freq_bands(cfg, cfg->detail, true);
                separate_freq_bands(cfg, cfg->detail, false);
            } else {
                fftw_execute(m_fftw_plan_l);
                separate_freq_bands(cfg, cfg->detail, true);
            }
        } else {

#ifdef DEBUG
            if (log_flag) {
                log_flag = false;
                debug("No sound for 3 seconds, sleeping.");
            }
#endif
            m_can_draw = false;
            return;
        }

        int diff;
        double diff_d;

        /* Additional filtering */
        if (cfg->filter_mode == source::FILTER_MCAT) {
            if (m_channels > 1) {
                apply_monstercat_filter(cfg, m_freq_l);
                apply_monstercat_filter(cfg, m_freq_r);
            } else {
                apply_monstercat_filter(cfg, m_freq_l);
            }
        } else if (cfg->filter_mode == source::FILTER_WAVES) {
            if (m_channels > 1) {
                apply_wave_filter(cfg, m_freq_l);
                apply_wave_filter(cfg, m_freq_r);
            } else {
                apply_wave_filter(cfg, m_freq_l);
            }
        }

        /* Copy frequencies over */
        memcpy(m_freq_both, m_freq_l, sizeof(int) * cfg->detail); /* left channel */
        if (m_channels > 1) /* right chanel */
            memcpy(m_freq_both + cfg->detail / 2, m_freq_r, sizeof(int) * cfg->detail);

        /* Processing */
        for (o = 0; o < cfg->detail * (m_channels > 1 ? 2 : 1); o++) {

            /* Falloff */
            if (m_current_gravity > 0) {
                if (m_freq_both[o] < m_last_freqs[o]) {
                    m_freq_both[o] = m_freq_peak[o] - (m_current_gravity * m_fall_off[o] * m_fall_off[o]);
                    m_fall_off[o]++;
                } else {
                    m_freq_peak[o] = m_freq_both[o];
                    m_fall_off[o] = 0;
                }
                /* This value has to be copied before other filters are applied */
                m_last_freqs[o] = m_freq_both[o];
            }

            /* integral (?) */
            if (cfg->integral > 0) {
                m_freq_both[o] = m_freq_mem[o] * cfg->integral + m_freq_both[o];
                m_freq_mem[o] = m_freq_both[o];

                diff = (cfg->bar_height + 1) - m_freq_both[o];
                if (diff < 0)
                    diff = 0;
                diff_d = 1. / (diff + 1);
                m_freq_mem[o] = m_freq_mem[o] * (1 - diff_d / 20);
            }

            /* Remove zero values, to prevent dividing by zero */
            if (m_freq_both[o] < 1)
                m_freq_both[o] = 1;

            /* TODO: automatic sensibility adjustment */
        }
    }

    void audio_processor::separate_freq_bands(source::config* cfg, uint16_t detail, bool left_channel)
    {
        int o, i;
        double peak, amplitude, tmp;
        for (o = 0; o < detail; o++) {
            peak = 0;
            for (i = 0; i < m_samples; i++) {
                //for (i = m_low_freq_cut[o]; i <= m_high_freq_cut[o]; i++) {
                if (left_channel)
                    amplitude = hypot(m_fftw_out_l[i][0], m_fftw_out_l[i][1]);
                else
                    amplitude = hypot(m_fftw_out_r[i][0], m_fftw_out_r[i][1]);
                peak += amplitude;
            }

            peak = peak / (m_high_freq_cut[o] - m_low_freq_cut[o] + 1); /* Averaging */
            tmp = peak / cfg->sens * m_freq_weight[o];

            if (tmp <= cfg->ignore)
                tmp = 0;

            if (left_channel)
                m_freq_l[o] = tmp;
            else
                m_freq_r[o] = tmp;
        }
    }

    void audio_processor::apply_monstercat_filter(source::config *cfg, int* t)
    {
        int m_y, de, z;

        for (z = 0; z < cfg->detail; z++) {
            for (m_y = z - 1; m_y >= 0; m_y--) {
                de = z - m_y;
                t[m_y] = UTIL_MAX(t[z] / pow(cfg->mcat_strength, de), t[m_y]);
            }

            for (m_y = z + 1; m_y < cfg->detail; m_y++) {
                de = m_y - z;
                t[m_y] = UTIL_MAX(t[z] / pow(cfg->mcat_strength, de), t[m_y]);
            }
        }
    }

    void audio_processor::apply_wave_filter(source::config *cfg, int* t) {
        int m_y, de, z;
        for (z = 0; z < cfg->detail; z++) {
            for (m_y = z - 1; m_y >= 0; m_y--) {
                de = z - m_y;
                t[m_y] = UTIL_MAX(t[z] - pow(2, de), t[m_y]);
            }

            for (m_y = z + 1; m_y < cfg->detail; m_y++) {
                de = m_y - z;
                t[m_y] = UTIL_MAX(t[z] - pow(2, de), t[m_y]);
            }
        }
    }

    uint8_t audio_processor::get_channels()
    {
        return m_channels;
    }

    int32_t audio_processor::get_buffer_size()
    {
        return m_buf_size;
    }

    int* audio_processor::get_freqs()
    {
        return m_freq_both;
    }

    int* audio_processor::get_last_freqs()
    {
        return m_last_freqsd;
    }

} /* namespace audio */
