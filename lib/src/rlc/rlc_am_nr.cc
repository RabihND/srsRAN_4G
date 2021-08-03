/**
 *
 * \section COPYRIGHT
 *
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * By using this file, you agree to the terms and conditions set
 * forth in the LICENSE file which can be found at the top level of
 * the distribution.
 *
 */

#include "srsran/rlc/rlc_am_nr.h"
#include "srsran/common/string_helpers.h"
#include "srsran/interfaces/ue_pdcp_interfaces.h"
#include "srsran/interfaces/ue_rrc_interfaces.h"
#include "srsran/rlc/rlc_am_nr_packing.h"
#include "srsran/srslog/event_trace.h"
#include <iostream>

namespace srsran {

/****************************************************************************
 * RLC AM NR entity
 ***************************************************************************/

/***************************************************************************
 *  Tx subclass implementation
 ***************************************************************************/
rlc_am_nr_tx::rlc_am_nr_tx(rlc_am* parent_) : parent(parent_), rlc_am_base_tx(&parent_->logger)
{
  parent->logger.debug("Initializing RLC AM NR TX: Tx_Next: %d",
                       st.tx_next); // Temporarly silence unused variable warning
}

bool rlc_am_nr_tx::configure(const rlc_config_t& cfg_)
{
  /*
    if (cfg_.tx_queue_length > MAX_SDUS_PER_RLC_PDU) {
      logger.error("Configuring Tx queue length of %d PDUs too big. Maximum value is %d.",
                   cfg_.tx_queue_length,
                   MAX_SDUS_PER_RLC_PDU);
      return false;
    }
  */
  cfg = cfg_.am;

  tx_enabled = true;

  return true;
}

bool rlc_am_nr_tx::has_data()
{
  return true;
}

uint32_t rlc_am_nr_tx::read_pdu(uint8_t* payload, uint32_t nof_bytes)
{
  std::lock_guard<std::mutex> lock(mutex);

  if (not tx_enabled) {
    logger->debug("RLC entity not active. Not generating PDU.");
    return 0;
  }

  logger->debug("MAC opportunity - %d bytes", nof_bytes);
  // logger.debug("tx_window size - %zu PDUs", tx_window.size());

  // Tx STATUS if requested
  // TODO

  // Section 5.2.2.3 in TS 36.311, if tx_window is full and retx_queue empty, retransmit PDU
  // TODO

  // RETX if required
  // TODO

  // Build a PDU from SDU
  unique_byte_buffer_t tx_sdu;
  unique_byte_buffer_t tx_pdu = srsran::make_byte_buffer();

  // Read new SDU from TX queue
  do {
    tx_sdu = tx_sdu_queue.read();
  } while (tx_sdu == nullptr && tx_sdu_queue.size() != 0);

  uint16_t hdr_size = 2;
  if (tx_sdu->N_bytes + hdr_size > nof_bytes) {
    logger->warning("Segmentation not supported yet");
    return 0;
  }
  rlc_am_nr_pdu_header_t hdr = {};
  hdr.dc                     = RLC_DC_FIELD_DATA_PDU;
  hdr.p                      = 0;
  hdr.si                     = rlc_nr_si_field_t::full_sdu;
  hdr.sn_size                = rlc_am_nr_sn_size_t::size12bits;
  hdr.sn                     = st.tx_next;

  uint32_t len = rlc_am_nr_write_data_pdu_header(hdr, tx_sdu.get());
  if (len > nof_bytes) {
    logger->error("Error writing AMD PDU header");
  }
  memcpy(payload, tx_sdu->msg, tx_sdu->N_bytes);
  return tx_sdu->N_bytes;
}

void rlc_am_nr_tx::handle_control_pdu(uint8_t* payload, uint32_t nof_bytes) {}

uint32_t rlc_am_nr_tx::get_buffer_state()
{
  uint32_t tx_queue      = 0;
  uint32_t prio_tx_queue = 0;
  get_buffer_state(tx_queue, prio_tx_queue);
  return tx_queue + prio_tx_queue;
}

void rlc_am_nr_tx::get_buffer_state(uint32_t& tx_queue, uint32_t& prio_tx_queue)
{
  std::lock_guard<std::mutex> lock(mutex);
  uint32_t                    n_bytes = 0;
  uint32_t                    n_sdus  = 0;

  /*
  logger.debug("%s Buffer state - do_status=%s, status_prohibit_running=%s (%d/%d)",
               rb_name,
               do_status() ? "yes" : "no",
               status_prohibit_timer.is_running() ? "yes" : "no",
               status_prohibit_timer.time_elapsed(),
               status_prohibit_timer.duration());
  */

  // Bytes needed for status report
  // TODO

  // Bytes needed for retx
  // TODO

  // Bytes needed for tx SDUs
  n_sdus = tx_sdu_queue.get_n_sdus();
  n_bytes += tx_sdu_queue.size_bytes();

  // Room needed for fixed header of data PDUs
  n_bytes += 2 * n_sdus; // TODO make header size configurable
  if (n_bytes > 0 && n_sdus > 0) {
    logger->debug("%s Total buffer state - %d SDUs (%d B)", rb_name, n_sdus, n_bytes);
  }

  tx_queue = n_bytes;
  return;
}

void rlc_am_nr_tx::reestablish()
{
  stop();
}

void rlc_am_nr_tx::discard_sdu(uint32_t discard_sn) {}

bool rlc_am_nr_tx::sdu_queue_is_full()
{
  return false;
}

void rlc_am_nr_tx::empty_queue() {}

void rlc_am_nr_tx::stop() {}

/****************************************************************************
 * Rx subclass implementation
 ***************************************************************************/
rlc_am_nr_rx::rlc_am_nr_rx(rlc_am* parent_) :
  parent(parent_), pool(byte_buffer_pool::get_instance()), rlc_am_base_rx(parent_, &parent_->logger)
{
  parent->logger.debug("Initializing RLC AM NR RX"); // Temporarly silence unused variable warning
}

bool rlc_am_nr_rx::configure(const rlc_config_t& cfg_)
{
  cfg = cfg_.am;

  return true;
}

void rlc_am_nr_rx::handle_data_pdu(uint8_t* payload, uint32_t nof_bytes)
{
  // Get AMD PDU Header
  rlc_am_nr_pdu_header_t header = {};
  rlc_am_nr_read_data_pdu_header(payload, nof_bytes, rlc_am_nr_sn_size_t::size12bits, &header);

  // Check poll bit
  if (header.p != 0) {
    logger->info("%s Status packet requested through polling bit", parent->rb_name);
    bool do_status = true;

    // 36.322 v10 Section 5.2.3
    // if (RX_MOD_BASE(header.sn) < RX_MOD_BASE(vr_ms) || RX_MOD_BASE(header.sn) >= RX_MOD_BASE(vr_mr)) {
    //  do_status = true;
    //}
    // else delay for reordering timer
  }
}

void rlc_am_nr_rx::stop() {}

void rlc_am_nr_rx::reestablish()
{
  stop();
}

uint32_t rlc_am_nr_rx::get_sdu_rx_latency_ms()
{
  return 0;
}

uint32_t rlc_am_nr_rx::get_rx_buffered_bytes()
{
  return 0;
}
} // namespace srsran
