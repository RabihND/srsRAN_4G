/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2015 Software Radio Systems Limited
 *
 * \section LICENSE
 *
 * This file is part of the srsUE library.
 *
 * srsUE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsUE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include <unistd.h>
#include <srslte/srslte.h>
#include "srslte/srslte.h"
#include "srslte/common/log.h"
#include "phy/phch_worker.h"
#include "phy/phch_common.h"
#include "phy/phch_recv.h"

#define Error(fmt, ...)   if (SRSLTE_DEBUG_ENABLED) log_h->error_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define Warning(fmt, ...) if (SRSLTE_DEBUG_ENABLED) log_h->warning_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define Info(fmt, ...)    if (SRSLTE_DEBUG_ENABLED) log_h->info_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)
#define Debug(fmt, ...)   if (SRSLTE_DEBUG_ENABLED) log_h->debug_line(__FILE__, __LINE__, fmt, ##__VA_ARGS__)

namespace srsue {


phch_recv::phch_recv() {
  running = false;
}

void phch_recv::init(srslte::radio_multi *_radio_handler, mac_interface_phy *_mac, rrc_interface_phy *_rrc,
                     prach *_prach_buffer, srslte::thread_pool *_workers_pool,
                     phch_common *_worker_com, srslte::log *_log_h, uint32_t nof_rx_antennas_, uint32_t prio,
                     int sync_cpu_affinity) {
  radio_h = _radio_handler;
  log_h = _log_h;
  mac = _mac;
  rrc = _rrc;
  workers_pool = _workers_pool;
  worker_com = _worker_com;
  prach_buffer = _prach_buffer;
  nof_rx_antennas = nof_rx_antennas_;

  tx_mutex_cnt = 0;
  running = true;
  phy_state = IDLE;
  time_adv_sec = 0;
  cell_is_set = false;
  sync_sfn_cnt = 0;
  srate_mode = SRATE_NONE;
  cell_search_in_progress = false;

  for (uint32_t i = 0; i < nof_rx_antennas; i++) {
    sf_buffer[i] = (cf_t *) srslte_vec_malloc(sizeof(cf_t) * 3 * SRSLTE_SF_LEN_PRB(100));
  }

  nof_tx_mutex = MUTEX_X_WORKER * workers_pool->get_nof_workers();
  worker_com->set_nof_mutex(nof_tx_mutex);
  if (sync_cpu_affinity < 0) {
    start(prio);
  } else {
    start_cpu(prio, sync_cpu_affinity);
  }


}

void phch_recv::stop() {
  running = false;
  wait_thread_finish();
  for (uint32_t i = 0; i < nof_rx_antennas; i++) {
    if (sf_buffer[i]) {
      free(sf_buffer[i]);
    }
  }
}

void phch_recv::set_agc_enable(bool enable) {
  do_agc = enable;
}

int radio_recv_wrapper_cs(void *h, cf_t *data[SRSLTE_MAX_PORTS], uint32_t nsamples, srslte_timestamp_t *rx_time) {
  srslte::radio_multi *radio_h = (srslte::radio_multi *) h;
  if (radio_h->rx_now(data, nsamples, rx_time)) {
    int offset = nsamples - radio_h->get_tti_len();
    if (abs(offset) < 10 && offset != 0) {
      radio_h->tx_offset(offset);
    } else if (nsamples < 10) {
      radio_h->tx_offset(nsamples);
    }
    return nsamples;
  } else {
    return -1;
  }
}

double callback_set_rx_gain(void *h, double gain) {
  srslte::radio_multi *radio_handler = (srslte::radio_multi *) h;
  return radio_handler->set_rx_gain_th(gain);
}

void phch_recv::set_time_adv_sec(float _time_adv_sec) {
  time_adv_sec = _time_adv_sec;
}

void phch_recv::set_ue_sync_opts(srslte_ue_sync_t *q) {
  if (worker_com->args->cfo_integer_enabled) {
    srslte_ue_sync_cfo_i_detec_en(q, true);
  }

  float cfo_tol = worker_com->args->cfo_correct_tol_hz;
  srslte_cfo_set_tol(&q->strack.cfocorr, cfo_tol / (15000 * q->fft_size));
  srslte_cfo_set_tol(&q->sfind.cfocorr, cfo_tol / (15000 * q->fft_size));

  int time_correct_period = worker_com->args->time_correct_period;
  if (time_correct_period > 0) {
    srslte_ue_sync_set_sample_offset_correct_period(q, time_correct_period);
  }

  sss_alg_t sss_alg = SSS_FULL;
  if (!worker_com->args->sss_algorithm.compare("diff")) {
    sss_alg = SSS_DIFF;
  } else if (!worker_com->args->sss_algorithm.compare("partial")) {
    sss_alg = SSS_PARTIAL_3;
  } else if (!worker_com->args->sss_algorithm.compare("full")) {
    sss_alg = SSS_FULL;
  } else {
    Warning("Invalid SSS algorithm %s. Using 'full'\n", worker_com->args->sss_algorithm.c_str());
  }
  srslte_sync_set_sss_algorithm(&q->strack, (sss_alg_t) sss_alg);
  srslte_sync_set_sss_algorithm(&q->sfind, (sss_alg_t) sss_alg);
}

bool phch_recv::init_cell() {
  cell_is_set = false;
  if (!srslte_ue_mib_init(&ue_mib, cell)) {
    if (!srslte_ue_sync_init_multi(&ue_sync, cell, radio_recv_wrapper_cs, nof_rx_antennas, radio_h)) {

      // Set options defined in expert section
      set_ue_sync_opts(&ue_sync);

      if (srslte_ue_dl_init_multi(&ue_dl_measure, cell, nof_rx_antennas)) {
        Error("Setting cell: initiating ue_dl_measure\n");
        return false;
      }

      for (uint32_t i = 0; i < workers_pool->get_nof_workers(); i++) {
        if (!((phch_worker *) workers_pool->get_worker(i))->init_cell(cell)) {
          Error("Setting cell: initiating PHCH worker\n");
          return false;
        }
      }
      radio_h->set_tti_len(SRSLTE_SF_LEN_PRB(cell.nof_prb));
      if (do_agc) {
        srslte_ue_sync_start_agc(&ue_sync, callback_set_rx_gain, last_gain);
      }
      srslte_ue_sync_set_cfo(&ue_sync, cellsearch_cfo);
      cell_is_set = true;
    } else {
      Error("Error setting cell: initiating ue_sync");
    }
  } else {
    Error("Error setting cell: initiating ue_mib\n");
  }
  return cell_is_set;
}

void phch_recv::free_cell() {
  if (phy_state != IDLE) {
    phy_state = IDLE;
    usleep(2000);
  }

  srslte_ue_sync_free(&ue_sync);

  srslte_ue_dl_free(&ue_dl_measure);

  if (cell_is_set) {
    for (uint32_t i = 0; i < workers_pool->get_nof_workers(); i++) {
      ((phch_worker *) workers_pool->get_worker(i))->free_cell();
    }
    prach_buffer->free_cell();
    cell_is_set = false;
  }
}


bool phch_recv::cell_search(int force_N_id_2) {
  uint8_t bch_payload[SRSLTE_BCH_PAYLOAD_LEN];
  uint8_t bch_payload_bits[SRSLTE_BCH_PAYLOAD_LEN / 8];

  srslte_ue_cellsearch_result_t found_cells[3];
  srslte_ue_cellsearch_t cs;

  bzero(found_cells, 3 * sizeof(srslte_ue_cellsearch_result_t));

  if (srslte_ue_cellsearch_init_multi(&cs, SRSLTE_DEFAULT_MAX_FRAMES_PSS, radio_recv_wrapper_cs, nof_rx_antennas,
                                      radio_h)) {
    Error("Initiating UE cell search\n");
    return false;
  }

  srslte_ue_cellsearch_set_nof_valid_frames(&cs, SRSLTE_DEFAULT_NOF_VALID_PSS_FRAMES);

  // Set options defined in expert section
  set_ue_sync_opts(&cs.ue_sync);

  if (do_agc) {
    srslte_ue_sync_start_agc(&cs.ue_sync, callback_set_rx_gain, last_gain);
  }

  if (srate_mode != SRATE_FIND) {
    srate_mode = SRATE_FIND;
    radio_h->set_rx_srate(1.92e6);
  }
  radio_h->start_rx();

  /* Find a cell in the given N_id_2 or go through the 3 of them to find the strongest */
  uint32_t max_peak_cell = 0;
  int ret = SRSLTE_ERROR;

  if (force_N_id_2 >= 0 && force_N_id_2 < 3) {
    ret = srslte_ue_cellsearch_scan_N_id_2(&cs, force_N_id_2, &found_cells[force_N_id_2]);
    max_peak_cell = force_N_id_2;
  } else {
    ret = srslte_ue_cellsearch_scan(&cs, found_cells, &max_peak_cell);
  }

  last_gain = srslte_agc_get_gain(&cs.ue_sync.agc);

  radio_h->stop_rx();
  srslte_ue_cellsearch_free(&cs);

  if (ret < 0) {
    Error("Error decoding MIB: Error searching PSS\n");
    return false;
  } else if (ret == 0) {
    Error("Error decoding MIB: Could not find any PSS in this frequency\n");
    return false;
  }

  // Save result
  cell.id = found_cells[max_peak_cell].cell_id;
  cell.cp = found_cells[max_peak_cell].cp;
  cellsearch_cfo = found_cells[max_peak_cell].cfo;

  srslte_ue_mib_sync_t ue_mib_sync;

  if (srslte_ue_mib_sync_init_multi(&ue_mib_sync, cell.id, cell.cp, radio_recv_wrapper_cs, nof_rx_antennas,
                                    radio_h)) {
    Error("Initiating UE MIB synchronization\n");
    return false;
  }

  // Set options defined in expert section
  set_ue_sync_opts(&ue_mib_sync.ue_sync);

  if (do_agc) {
    srslte_ue_sync_start_agc(&ue_mib_sync.ue_sync, callback_set_rx_gain, last_gain);
  }

  srslte_ue_sync_set_cfo(&ue_mib_sync.ue_sync, cellsearch_cfo);

  /* Find and decode MIB */
  uint32_t sfn;
  int sfn_offset;
  radio_h->start_rx();
  ret = srslte_ue_mib_sync_decode(&ue_mib_sync,
                                  SRSLTE_DEFAULT_MAX_FRAMES_PBCH,
                                  bch_payload, &cell.nof_ports, &sfn_offset);
  radio_h->stop_rx();
  last_gain = srslte_agc_get_gain(&ue_mib_sync.ue_sync.agc);
  cellsearch_cfo = srslte_ue_sync_get_cfo(&ue_mib_sync.ue_sync);
  srslte_ue_mib_sync_free(&ue_mib_sync);

  if (ret == 1) {
    srslte_pbch_mib_unpack(bch_payload, &cell, NULL);
    worker_com->set_cell(cell);
    return true;
  } else {
    Warning("Error decoding MIB: Error decoding PBCH\n");
    return false;
  }
}


void phch_recv::resync_sfn() {
  sync_sfn_cnt = 0;
  phy_state = CELL_SELECT;
}

void phch_recv::set_earfcn(std::vector<uint32_t> earfcn) {
  this->earfcn = earfcn;
}

void phch_recv::cell_search_next() {
  cell_search_in_progress = true;
  cur_earfcn_index++;
  if (cur_earfcn_index >= 0) {
    if (cur_earfcn_index >= (int) earfcn.size() - 1) {
      cur_earfcn_index = 0;
    }
    // If PHY is running, stop and free resources
    free_cell();

    float dl_freq = 1e6*srslte_band_fd(earfcn[cur_earfcn_index]);
    if (dl_freq >= 0) {
      log_h->info("Cell Search: Set DL EARFCN=%d, frequency=%.1f MHz, channel_index=%d\n", earfcn[cur_earfcn_index],
                  dl_freq / 1e6, cur_earfcn_index);
      radio_h->set_rx_freq(dl_freq);

      // Start PHY cell search (finds maximum cell in frequency)
      phy_state = CELL_SEARCH;
    } else {
      log_h->error("Cell Search: Invalid EARFCN=%d, channel_index=%d\n", earfcn[cur_earfcn_index], cur_earfcn_index);
    }
  }
}

void phch_recv::cell_search_start() {
  if (earfcn.size() > 0) {
    cur_earfcn_index = -1;
    log_h->console("Starting Cell Search procedure in %d EARFCNs...\n", earfcn.size());
    log_h->info("Cell Search: Starting procedure...\n");
    cell_search_next();
  } else {
    log_h->info("Empty EARFCN list. Stopping cell search...\n");
    log_h->console("Empty EARFCN list. Stopping cell search...\n");
  }
}

bool phch_recv::cell_select(uint32_t earfcn, srslte_cell_t cell) {
  free_cell();

  int cnt=0;
  while(phy_state == CELL_SEARCH && cnt<100) {
    usleep(10000);
    log_h->info("PHY in CELL_SEARCH. Waiting...\n");
  }
  if (phy_state==CELL_SEARCH) {
    log_h->warning("PHY still in CELL_SEARCH, forcing CELL_SELECT...\n");
  }
  float dl_freq = 1e6*srslte_band_fd(earfcn);
  float ul_freq = 1e6*srslte_band_fu(srslte_band_ul_earfcn(earfcn));
  if (dl_freq >= 0 || ul_freq <= 0) {
    log_h->info("Cell Select: Set EARFCN=%d, frequency=%.1f MHz, UL frequency=%.1f MHz\n", earfcn, dl_freq / 1e6,
                ul_freq / 1e6);
    radio_h->set_rx_freq(dl_freq);
    radio_h->set_tx_freq(ul_freq);

    ul_dl_factor = ul_freq/dl_freq;

    cell_search_in_progress = false;
    this->cell = cell;
    if (init_cell()) {
      phy_state = CELL_SELECT;
      return true;
    } else {
      log_h->error("Cell Select: Initializing cell in EARFCN=%d, PCI=%d\n", earfcn, cell.id);
    }
  } else {
    log_h->error("Cell Select: Invalid EARFCN=%d\n", earfcn);
  }
  return false;
}


int phch_recv::cell_sync_sfn(void) {

  int ret = SRSLTE_ERROR;
  uint8_t bch_payload[SRSLTE_BCH_PAYLOAD_LEN];

  srslte_ue_sync_decode_sss_on_track(&ue_sync, true);
  ret = srslte_ue_sync_zerocopy_multi(&ue_sync, sf_buffer);
  if (ret < 0) {
    Error("Error calling ue_sync_get_buffer");
    return -1;
  }

  if (ret == 1) {
    if (srslte_ue_sync_get_sfidx(&ue_sync) == 0) {
      int sfn_offset = 0;
      Info("SYNC:  Decoding MIB...\n");
      int n = srslte_ue_mib_decode(&ue_mib, sf_buffer[0], bch_payload, NULL, &sfn_offset);
      if (n < 0) {
        Error("SYNC:  Error decoding MIB while synchronising SFN");
        return -1;
      } else if (n == SRSLTE_UE_MIB_FOUND) {
        uint32_t sfn;
        srslte_pbch_mib_unpack(bch_payload, &cell, &sfn);

        sfn = (sfn+sfn_offset)%1024;
        tti = sfn * 10;

        srslte_ue_sync_decode_sss_on_track(&ue_sync, true);
        Info("SYNC:  DONE, TTI=%d, sfn_offset=%d\n", tti, sfn_offset);
        srslte_ue_mib_reset(&ue_mib);
        return 1;
      }
    }
  } else {
    Debug("SYNC:  PSS/SSS not found...\n");
  }
  return 0;
}

int phch_recv::cell_meas_rsrp() {

  uint32_t cfi = 0;

  tti = (tti+1) % 10240;
  log_h->step(tti);

  uint32_t sf_idx = tti%10;

  int sync_res = srslte_ue_sync_zerocopy_multi(&ue_sync, sf_buffer);
  if (sync_res == 1) {
    if (srslte_ue_dl_decode_fft_estimate_multi(&ue_dl_measure, sf_buffer, sf_idx, &cfi)) {
      log_h->error("SYNC:  Measuring RSRP: Estimating channel\n");
      return -1;
    }
    float rsrp   = srslte_chest_dl_get_rsrp(&ue_dl_measure.chest);
    measure_rsrp = SRSLTE_VEC_CMA(rsrp, measure_rsrp, measure_cnt);
    measure_cnt++;
    log_h->info("SYNC:  Measuring RSRP %d/%d, sf_idx=%d, RSRP=%.1f dBm\n",
                measure_cnt, RSRP_MEASURE_NOF_FRAMES, sf_idx, 10 * log10(rsrp / 1000));
    if (measure_cnt >= RSRP_MEASURE_NOF_FRAMES) {
      return 1;
    }
  } else {
    log_h->error("SYNC:  Measuring RSRP: Sync error\n");
    return -1;
  }

  return 0;
}

void phch_recv::run_thread() {
  int sync_res;
  phch_worker *worker = NULL;
  cf_t *buffer[SRSLTE_MAX_PORTS];
  while (running) {
    switch (phy_state) {
      case CELL_SEARCH:
        if (cell_search()) {
          init_cell();
          float srate = (float) srslte_sampling_freq_hz(cell.nof_prb);

          if (30720 % ((int) srate / 1000) == 0) {
            radio_h->set_master_clock_rate(30.72e6);
          } else {
            radio_h->set_master_clock_rate(23.04e6);
          }

          log_h->info("Setting Sampling frequency %.2f MHz\n", (float) srate / 1000000);
          srate_mode = SRATE_CAMP;
          radio_h->set_rx_srate(srate);
          radio_h->set_tx_srate(srate);

          Info("SYNC:  Cell found. Synchronizing...\n");
          phy_state = CELL_SELECT;
          sync_sfn_cnt = 0;
          srslte_ue_mib_reset(&ue_mib);
        }
        break;
      case CELL_SELECT:

        srslte_ue_sync_decode_sss_on_track(&ue_sync, true);

        if (!radio_is_streaming) {
          // Start streaming
          radio_h->start_rx();
          radio_is_streaming = true;
        }

        switch (cell_sync_sfn()) {
          default:
            log_h->console("Going IDLE\n");
            phy_state = IDLE;
            break;
          case 1:
            srslte_ue_sync_set_agc_period(&ue_sync, 20);
            if (!cell_search_in_progress) {
              phy_state = CELL_CAMP;
              log_h->console("Sync OK. Camping on cell PCI=%d...\n", cell.id);
            } else {
              measure_cnt  = 0;
              measure_rsrp = 0;
              phy_state    = CELL_MEASURE;
            }
            break;
          case 0:
            break;
        }
        sync_sfn_cnt++;
        if (sync_sfn_cnt >= SYNC_SFN_TIMEOUT) {
          sync_sfn_cnt = 0;
          radio_h->stop_rx();
          radio_is_streaming = false;
          log_h->console("Timeout while synchronizing SFN\n");
          log_h->warning("Timeout while synchronizing SFN\n");
        }
        break;
      case CELL_MEASURE:
        switch(cell_meas_rsrp()) {
          case 1:
            rrc->cell_found(earfcn[cur_earfcn_index], cell, 10*log10(measure_rsrp/1000));
            phy_state = CELL_CAMP;
          case 0:
            break;
          default:
            log_h->error("SYNC:  Getting RSRP cell measurement.\n");
            cell_search_next();
        }
        break;
      case CELL_CAMP:
        tti = (tti+1) % 10240;
        worker = (phch_worker *) workers_pool->wait_worker(tti);
        if (worker) {
          for (uint32_t i = 0; i < nof_rx_antennas; i++) {
            buffer[i] = worker->get_buffer(i);
          }

          sync_res = srslte_ue_sync_zerocopy_multi(&ue_sync, buffer);
          if (sync_res == 1) {

            log_h->step(tti);

            Debug("Worker %d synchronized\n", worker->get_id());

            metrics.sfo = srslte_ue_sync_get_sfo(&ue_sync);
            metrics.cfo = srslte_ue_sync_get_cfo(&ue_sync);
            worker->set_cfo(ul_dl_factor * metrics.cfo / 15000);
            worker_com->set_sync_metrics(metrics);

            float sample_offset = (float) srslte_ue_sync_get_sfo(&ue_sync) / 1000;
            worker->set_sample_offset(sample_offset);

            /* Compute TX time: Any transmission happens in TTI4 thus advance 4 ms the reception time */
            srslte_timestamp_t rx_time, tx_time, tx_time_prach;
            srslte_ue_sync_get_last_timestamp(&ue_sync, &rx_time);
            srslte_timestamp_copy(&tx_time, &rx_time);
            srslte_timestamp_add(&tx_time, 0, 4e-3 - time_adv_sec);
            worker->set_tx_time(tx_time);

            Debug("Settting TTI=%d, tx_mutex=%d to worker %d\n", tti, tx_mutex_cnt, worker->get_id());
            worker->set_tti(tti, tx_mutex_cnt);
            tx_mutex_cnt = (tx_mutex_cnt+1) % nof_tx_mutex;

            // Check if we need to TX a PRACH
            if (prach_buffer->is_ready_to_send(tti)) {
              srslte_timestamp_copy(&tx_time_prach, &rx_time);
              srslte_timestamp_add(&tx_time_prach, 0, prach::tx_advance_sf * 1e-3);
              prach_buffer->send(radio_h, ul_dl_factor * metrics.cfo / 15000, worker_com->pathloss, tx_time_prach);
              radio_h->tx_end();
              worker_com->p0_preamble = prach_buffer->get_p0_preamble();
              worker_com->cur_radio_power = SRSLTE_MIN(SRSLTE_PC_MAX, worker_com->pathloss+worker_com->p0_preamble);
            }
            workers_pool->start_worker(worker);
            // Notify RRC in-sync every 1 frame
            if ((tti % 10) == 0) {
              rrc->in_sync();
              log_h->debug("Sending in-sync to RRC\n");
            }
          } else {
            log_h->console("Sync error.\n");
            log_h->error("Sync error. Sending out-of-sync to RRC\n");
            // Notify RRC of out-of-sync frame
            rrc->out_of_sync();
            worker->release();
            worker_com->reset_ul();
            phy_state = CELL_SELECT;
          }
        } else {
          // wait_worker() only returns NULL if it's being closed. Quit now to avoid unnecessary loops here
          running = false;
        }
        break;
      case IDLE:
        usleep(1000);
        break;
    }
  }
}

uint32_t phch_recv::get_current_tti() {
  return tti;
}

bool phch_recv::status_is_sync() {
  return phy_state == CELL_CAMP;
}

void phch_recv::get_current_cell(srslte_cell_t *cell_) {
  if (cell_) {
    memcpy(cell_, &cell, sizeof(srslte_cell_t));
  }
}
}
