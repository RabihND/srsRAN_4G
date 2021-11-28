/**
 * Copyright 2013-2021 Software Radio Systems Limited
 *
 * This file is part of srsRAN.
 *
 * srsRAN is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsRAN is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#ifndef SRSRAN_RRC_NR_TEST_HELPERS_H
#define SRSRAN_RRC_NR_TEST_HELPERS_H

#include "srsenb/test/common/dummy_classes_common.h"
#include "srsgnb/hdr/stack/common/test/dummy_nr_classes.h"
#include "srsgnb/hdr/stack/rrc/rrc_nr.h"

namespace srsenb {

class pdcp_nr_rrc_tester : public pdcp_dummy
{
public:
  void write_sdu(uint16_t rnti, uint32_t lcid, srsran::unique_byte_buffer_t sdu, int pdcp_sn = -1) override
  {
    last_sdu_rnti = rnti;
    last_sdu_lcid = lcid;
    last_sdu      = std::move(sdu);
  }

  uint16_t                     last_sdu_rnti = SRSRAN_INVALID_RNTI;
  uint32_t                     last_sdu_lcid = srsran::MAX_NR_NOF_BEARERS;
  srsran::unique_byte_buffer_t last_sdu;
};

class rlc_nr_rrc_tester : public rlc_dummy
{
public:
  void write_sdu(uint16_t rnti, uint32_t lcid, srsran::unique_byte_buffer_t sdu)
  {
    last_sdu_rnti = rnti;
    last_sdu_lcid = lcid;
    last_sdu      = std::move(sdu);
  }

  uint16_t                     last_sdu_rnti;
  uint32_t                     last_sdu_lcid;
  srsran::unique_byte_buffer_t last_sdu;
};

/**
 * Run TS 38.331, 5.3.3 "RRC connection establishment" to completion
 * RRC actions:
 * - Rx RRCSetupRequest
 * - Tx RRCSetup to lower layers
 * - Tx RRCSetupComplete
 * Checks:
 * - the RRC sends RRCSetup as reply to RRCSetupRequest
 * - verify that RRCSetup rnti, lcid are correct
 * - verify that RRCSetup adds an SRB1
 */
void test_rrc_nr_connection_establishment(srsran::task_scheduler& task_sched,
                                          rrc_nr&                 rrc_obj,
                                          rlc_nr_rrc_tester&      rlc,
                                          mac_nr_dummy&           mac,
                                          uint16_t                rnti);

void test_rrc_nr_security_mode_cmd(srsran::task_scheduler& task_sched,
                                   rrc_nr&                 rrc_obj,
                                   pdcp_nr_rrc_tester&     pdcp,
                                   uint16_t                rnti);

} // namespace srsenb

#endif // SRSRAN_RRC_NR_TEST_HELPERS_H
