/*
 * Copyright (c) 2017 UAVCAN Team
 *
 * Distributed under the MIT License, available in the file LICENSE.
 *
 * Author: Pavel Kirienko <pavel.kirienko@zubax.com>
 */

#include "canard_stm32.h"
#include "_internal_bxcan.h"
#include <unistd.h>


#if CANARD_STM32_USE_CAN2
# define BXCAN                                                  CANARD_STM32_CAN2
#else
# define BXCAN                                                  CANARD_STM32_CAN1
#endif

/*
 * State variables
 */
static CanardSTM32Stats g_stats;

static bool g_abort_tx_on_error = false;

void CAN_RX0_IRQHandler(void) {
    extern void CAN_IT_Callback();

    if ((CAN->IER & CAN_IER_FMPIE0) && (CAN->RF0R & 0x3)) {
        CAN_IT_Callback();

    } else {
        if ((CAN->ESR & CAN_ESR_LEC) && (CAN->IER & CAN_IER_LECIE) && (CAN->IER & CAN_IER_ERRIE)) {
            /* TODO FE-5184 See if this is necessary */
            /* Canard is supposed to handle error codes but we may have a leftover one at startup. */

            CAN->ESR &= ~CAN_ESR_LEC;
        }

        CAN->MSR |= CAN_MSR_ERRI;
    }
}

void CAN_TX_IRQHandler(void) {
    extern void processTxQueue();

    /* Check End of transmission flag */
    if ((CAN->IER & CAN_IER_TMEIE) == CAN_IER_TMEIE)
    {
        /* Check Transmit request completion status */
        if ((((CAN->TSR) & (CAN_TSR_RQCP0 | CAN_TSR_TXOK0 | CAN_TSR_TME0)) == (CAN_TSR_RQCP0 | CAN_TSR_TXOK0 | CAN_TSR_TME0))
            || (((CAN->TSR) & (CAN_TSR_RQCP1 | CAN_TSR_TXOK1 | CAN_TSR_TME1)) == (CAN_TSR_RQCP1 | CAN_TSR_TXOK1 | CAN_TSR_TME1))
            || (((CAN->TSR) & (CAN_TSR_RQCP2 | CAN_TSR_TXOK2 | CAN_TSR_TME2)) == (CAN_TSR_RQCP2 | CAN_TSR_TXOK2 | CAN_TSR_TME2)))
        {
            /* Check Transmit success */
            if ((((CAN->TSR) & CAN_TSR_TXOK0) == CAN_TSR_TXOK0)
                || (((CAN->TSR) & CAN_TSR_TXOK1) == CAN_TSR_TXOK1)
                || (((CAN->TSR) & CAN_TSR_TXOK2) == CAN_TSR_TXOK2))
            {
                processTxQueue();
            }
            else /* Transmit failure */
            {
                /* Set CAN error code to TXFAIL error */
                canard_errors.tx_errors++;
            }

            /* Clear transmission status flags (RQCPx and TXOKx) */
            CAN->TSR |= CAN_TSR_RQCP0  | CAN_TSR_RQCP1  | CAN_TSR_RQCP2 | CAN_TSR_TXOK0 | CAN_TSR_TXOK1 | CAN_TSR_TXOK2;
        }
    }
}

static bool isFramePriorityHigher(uint32_t a, uint32_t b)
{
    const uint32_t clean_a = a & CANARD_CAN_EXT_ID_MASK;
    const uint32_t clean_b = b & CANARD_CAN_EXT_ID_MASK;

    /*
     * STD vs EXT - if 11 most significant bits are the same, EXT loses.
     */
    const bool ext_a = (a & CANARD_CAN_FRAME_EFF) != 0;
    const bool ext_b = (b & CANARD_CAN_FRAME_EFF) != 0;
    if (ext_a != ext_b)
    {
        const uint32_t arb11_a = ext_a ? (clean_a >> 18) : clean_a;
        const uint32_t arb11_b = ext_b ? (clean_b >> 18) : clean_b;
        if (arb11_a != arb11_b)
        {
            return arb11_a < arb11_b;
        }
        else
        {
            return ext_b;
        }
    }

    /*
     * RTR vs Data frame - if frame identifiers and frame types are the same, RTR loses.
     */
    const bool rtr_a = (a & CANARD_CAN_FRAME_RTR) != 0;
    const bool rtr_b = (b & CANARD_CAN_FRAME_RTR) != 0;
    if ((clean_a == clean_b) && (rtr_a != rtr_b))
    {
        return rtr_b;
    }

    /*
     * Plain ID arbitration - greater value loses.
     */
    return clean_a < clean_b;
}

/// Converts libcanard ID value into the bxCAN TX ID register format.
static uint32_t convertFrameIDCanardToRegister(const uint32_t id)
{
    uint32_t out = 0;

    if (id & CANARD_CAN_FRAME_EFF)
    {
        out = ((id & CANARD_CAN_EXT_ID_MASK) << 3) | CANARD_STM32_CAN_TIR_IDE;
    }
    else
    {
        out = ((id & CANARD_CAN_STD_ID_MASK) << 21);
    }

    if (id & CANARD_CAN_FRAME_RTR)
    {
        out |= CANARD_STM32_CAN_TIR_RTR;
    }

    return out;
}

/// Converts bxCAN TX/RX (sic! both RX/TX are supported) ID register value into the libcanard ID format.
static uint32_t convertFrameIDRegisterToCanard(const uint32_t id)
{
#if (CANARD_STM32_CAN_TIR_RTR != CANARD_STM32_CAN_RIR_RTR) ||\
    (CANARD_STM32_CAN_TIR_IDE != CANARD_STM32_CAN_RIR_IDE)
# error "RIR bits do not match TIR bits, TIR --> libcanard conversion is not possible"
#endif

    uint32_t out = 0;

    if ((id & CANARD_STM32_CAN_RIR_IDE) == 0)
    {
        out = (CANARD_CAN_STD_ID_MASK & (id >> 21));
    }
    else
    {
        out = (CANARD_CAN_EXT_ID_MASK & (id >> 3)) | CANARD_CAN_FRAME_EFF;
    }

    if ((id & CANARD_STM32_CAN_RIR_RTR) != 0)
    {
        out |= CANARD_CAN_FRAME_RTR;
    }

    return out;
}


static bool waitMSRINAKBitStateChange(volatile const CanardSTM32CANType* const bxcan, const bool target_state)
{
    /**
     * A properly functioning bus will exhibit 11 consecutive recessive bits at the end of every correct transmission,
     * or while the bus is idle. The 11 consecutive recessive bits are made up of:
     *  1 bit - acknowledgement delimiter
     *  7 bit - end of frame bits
     *  3 bit - inter frame space
     * This adds up to 11; therefore, it is not really necessary to wait longer than a few frame TX intervals.
     */
    static const unsigned TimeoutMilliseconds = 1000;

    for (unsigned wait_ack = 0; wait_ack < TimeoutMilliseconds; wait_ack++)
    {
        const bool state = (bxcan->MSR & CANARD_STM32_CAN_MSR_INAK) != 0;
        if (state == target_state)
        {
            return true;
        }

        // Sleep 1 millisecond
        usleep(1000);           // TODO: This function may be missing on some platforms
    }

    return false;
}


static void processErrorStatus(void)
{
    /*
     * Aborting TX transmissions if abort on error was requested
     * Updating error counter
     */
    const uint8_t lec = (uint8_t)((BXCAN->ESR & CANARD_STM32_CAN_ESR_LEC_MASK) >> CANARD_STM32_CAN_ESR_LEC_SHIFT);

    if (lec != 0)
    {
        BXCAN->ESR = 0;                 // This action does only affect the LEC bits, other bits are read only!
        g_stats.error_count++;

        // Abort pending transmissions if auto abort on error is enabled, or if we're in bus off mode
        if (g_abort_tx_on_error || (BXCAN->ESR & CANARD_STM32_CAN_ESR_BOFF))
        {
            BXCAN->TSR = CANARD_STM32_CAN_TSR_ABRQ0 | CANARD_STM32_CAN_TSR_ABRQ1 | CANARD_STM32_CAN_TSR_ABRQ2;
        }
    }
}


int canardSTM32Init(const CanardSTM32CANTimings* const timings,
                    const CanardSTM32IfaceMode iface_mode)
{
    /*
     * Paranoia time.
     */
    if ((iface_mode != CanardSTM32IfaceModeNormal) &&
        (iface_mode != CanardSTM32IfaceModeSilent) &&
        (iface_mode != CanardSTM32IfaceModeAutomaticTxAbortOnError))
    {
        return -CANARD_ERROR_INVALID_ARGUMENT;
    }

    if ((timings == NULL) ||
        (timings->bit_rate_prescaler < 1) || (timings->bit_rate_prescaler > 1024) ||
        (timings->max_resynchronization_jump_width < 1) || (timings->max_resynchronization_jump_width > 4) ||
        (timings->bit_segment_1 < 1) || (timings->bit_segment_1 > 16) ||
        (timings->bit_segment_2 < 1) || (timings->bit_segment_2 > 8))
    {
        return -CANARD_ERROR_INVALID_ARGUMENT;
    }

    /*
     * Initial setup
     */
    memset(&g_stats, 0, sizeof(g_stats));

    g_abort_tx_on_error = (iface_mode == CanardSTM32IfaceModeAutomaticTxAbortOnError);

#if CANARD_STM32_USE_CAN2
    // If we're using CAN2, we MUST initialize CAN1 first, because CAN2 is a slave to CAN1.
    CANARD_STM32_CAN1->IER = 0;                                 // We need no interrupts
    CANARD_STM32_CAN1->MCR &= ~CANARD_STM32_CAN_MCR_SLEEP;      // Exit sleep mode
    CANARD_STM32_CAN1->MCR |= CANARD_STM32_CAN_MCR_INRQ;        // Request init

    if (!waitMSRINAKBitStateChange(CANARD_STM32_CAN1, true))    // Wait for synchronization
    {
        CANARD_STM32_CAN1->MCR = CANARD_STM32_CAN_MCR_RESET;
        return -CANARD_STM32_ERROR_MSR_INAK_NOT_SET;
    }
    // CAN1 will be left in the initialization mode forever, in this mode it does not affect the bus at all.
#endif

    BXCAN->IER = 0;                                             // We need no interrupts
    BXCAN->MCR &= ~CANARD_STM32_CAN_MCR_SLEEP;                  // Exit sleep mode
    BXCAN->MCR |= CANARD_STM32_CAN_MCR_INRQ;                    // Request init

    if (!waitMSRINAKBitStateChange(BXCAN, true))                // Wait for synchronization
    {
        BXCAN->MCR = CANARD_STM32_CAN_MCR_RESET;
        return -CANARD_STM32_ERROR_MSR_INAK_NOT_SET;
    }

    /*
     * Hardware initialization (the hardware has already confirmed initialization mode, see above)
     */
    BXCAN->MCR = CANARD_STM32_CAN_MCR_ABOM | CANARD_STM32_CAN_MCR_AWUM | CANARD_STM32_CAN_MCR_INRQ;  // RM page 648

    BXCAN->BTR = (((timings->max_resynchronization_jump_width - 1) &    3U) << 24) |
                 (((timings->bit_segment_1 - 1)                    &   15U) << 16) |
                 (((timings->bit_segment_2 - 1)                    &    7U) << 20) |
                 ((timings->bit_rate_prescaler - 1)                & 1023U) |
                 ((iface_mode == CanardSTM32IfaceModeSilent) ? CANARD_STM32_CAN_BTR_SILM : 0);

    CANARD_ASSERT(0 == BXCAN->IER);             // Making sure the iterrupts are indeed disabled

    BXCAN->MCR &= ~CANARD_STM32_CAN_MCR_INRQ;   // Leave init mode

    // Enable mailbox empty (TX callback) interrupt
    BXCAN->IER |= CANARD_STM32_CAN_IER_TMEIE;

    if (!waitMSRINAKBitStateChange(BXCAN, false))
    {
        BXCAN->MCR = CANARD_STM32_CAN_MCR_RESET;
        return -CANARD_STM32_ERROR_MSR_INAK_NOT_CLEARED;
    }

    /*
     * Default filter configuration. Note that ALL filters are available ONLY via CAN1!
     * CAN2 filters are offset by 14.
     * We use 14 filters at most always which simplifies the code and ensures compatibility with all
     * MCU within the STM32 family.
     */
    {
        uint32_t fmr = CANARD_STM32_CAN1->FMR & 0xFFFFC0F1;
        fmr |= CANARD_STM32_NUM_ACCEPTANCE_FILTERS << 8;                // CAN2 start bank = 14 (if CAN2 is present)
        CANARD_STM32_CAN1->FMR = fmr | CANARD_STM32_CAN_FMR_FINIT;
    }

    CANARD_ASSERT(((CANARD_STM32_CAN1->FMR >> 8) & 0x3F) == CANARD_STM32_NUM_ACCEPTANCE_FILTERS);

    CANARD_STM32_CAN1->FM1R = 0;                                        // Indentifier Mask mode
    CANARD_STM32_CAN1->FS1R = 0x0FFFFFFF;                               // All 32-bit

    // Filters are alternating between FIFO0 and FIFO1 in order to equalize the load.
    // This will cause occasional priority inversion and frame reordering on reception,
    // but that is acceptable for UAVCAN, and a majority of other protocols will tolerate
    // this too, since there will be no reordering within the same CAN ID.
    CANARD_STM32_CAN1->FFA1R = 0x0AAAAAAA;

#if CANARD_STM32_USE_CAN2
    CANARD_STM32_CAN1->FilterRegister[CANARD_STM32_NUM_ACCEPTANCE_FILTERS].FR1 = 0;
    CANARD_STM32_CAN1->FilterRegister[CANARD_STM32_NUM_ACCEPTANCE_FILTERS].FR2 = 0;
    CANARD_STM32_CAN1->FA1R = (1 << CANARD_STM32_NUM_ACCEPTANCE_FILTERS);  // One filter enabled
#else
    CANARD_STM32_CAN1->FilterRegister[0].FR1 = 0;
    CANARD_STM32_CAN1->FilterRegister[0].FR2 = 0;
    CANARD_STM32_CAN1->FA1R = 1;                                        // One filter enabled
#endif

    CANARD_STM32_CAN1->FMR &= ~CANARD_STM32_CAN_FMR_FINIT;              // Leave initialization mode

    return 0;
}


int canardSTM32Transmit(const CanardCANFrame* const frame)
{
    if (frame == NULL)
    {
        return -CANARD_ERROR_INVALID_ARGUMENT;
    }

    if (frame->id & CANARD_CAN_FRAME_ERR)
    {
        return -CANARD_STM32_ERROR_UNSUPPORTED_FRAME_FORMAT;
    }

    /*
     * Handling error status might free up some slots through aborts
     */
    processErrorStatus();

    /*
     * Seeking an empty slot, checking if priority inversion would occur if we enqueued now.
     * We can always enqueue safely if all TX mailboxes are free and no transmissions are pending.
     */
    uint8_t tx_mailbox = 0xFF;

    static const uint32_t AllTME = CANARD_STM32_CAN_TSR_TME0 | CANARD_STM32_CAN_TSR_TME1 | CANARD_STM32_CAN_TSR_TME2;

    if ((BXCAN->TSR & AllTME) != AllTME)                // At least one TX mailbox is used, detailed check is needed
    {
        const bool tme[3] =
        {
            (BXCAN->TSR & CANARD_STM32_CAN_TSR_TME0) != 0,
            (BXCAN->TSR & CANARD_STM32_CAN_TSR_TME1) != 0,
            (BXCAN->TSR & CANARD_STM32_CAN_TSR_TME2) != 0
        };

        for (uint8_t i = 0; i < 3; i++)
        {
            if (tme[i])                                 // This TX mailbox is free, we can use it
            {
                tx_mailbox = i;
            }
            else                                        // This TX mailbox is pending, check for priority inversion
            {
                if (!isFramePriorityHigher(frame->id, convertFrameIDRegisterToCanard(BXCAN->TxMailbox[i].TIR)))
                {
                    // There's a mailbox whose priority is higher or equal the priority of the new frame.
                    return 0;                           // Priority inversion would occur! Reject transmission.
                }
            }
        }

        if (tx_mailbox == 0xFF)
        {
            /*
             * All TX mailboxes are busy (this is highly unlikely); at the same time we know that there is no
             * higher or equal priority frame that is currently pending. Therefore, priority inversion has
             * just happend (sic!), because we can't enqueue the higher priority frame due to all TX mailboxes
             * being busy. This scenario is extremely unlikely, because in order for it to happen, the application
             * would need to transmit 4 (four) or more CAN frames with different CAN ID ordered from high ID to
             * low ID nearly at the same time. For example:
             *  1. 0x123        <-- Takes mailbox 0 (or any other)
             *  2. 0x122        <-- Takes mailbox 2 (or any other)
             *  3. 0x121        <-- Takes mailbox 1 (or any other)
             *  4. 0x120        <-- INNER PRIORITY INVERSION HERE (only if all three TX mailboxes are still busy)
             * This situation is even less likely to cause any noticeable disruptions on the CAN bus. Despite that,
             * it is better to warn the developer about that during debugging, so we fire an assertion failure here.
             * It is perfectly safe to remove it.
             */
#if CANARD_STM32_DEBUG_INNER_PRIORITY_INVERSION
            CANARD_ASSERT(!"CAN PRIO INV");
#endif
            return 0;
        }
    }
    else                                                // All TX mailboxes are free, use first
    {
        tx_mailbox = 0;
    }

    CANARD_ASSERT(tx_mailbox < 3);                      // Index check - the value must be correct here

    /*
     * By this time we've proved that a priority inversion would not occur, and we've also found a free TX mailbox.
     * Therefore it is safe to enqueue the frame now.
     */
    volatile CanardSTM32TxMailboxType* const mb = &BXCAN->TxMailbox[tx_mailbox];

    mb->TDTR = frame->data_len;                         // DLC equals data length except in CAN FD

    mb->TDHR = (((uint32_t)frame->data[7]) << 24) |
               (((uint32_t)frame->data[6]) << 16) |
               (((uint32_t)frame->data[5]) <<  8) |
               (((uint32_t)frame->data[4]) <<  0);
    mb->TDLR = (((uint32_t)frame->data[3]) << 24) |
               (((uint32_t)frame->data[2]) << 16) |
               (((uint32_t)frame->data[1]) <<  8) |
               (((uint32_t)frame->data[0]) <<  0);

    mb->TIR = convertFrameIDCanardToRegister(frame->id) | CANARD_STM32_CAN_TIR_TXRQ;    // Go.

    /*
     * The frame is now enqueued and pending transmission.
     */
    return 1;
}

void canardSTM32Receive_IT() {
    /* Enable interrupts: */
    /*  - Enable Error warning Interrupt */
    /*  - Enable Error passive Interrupt */
    /*  - Enable Bus-off Interrupt */
    /*  - Enable Last error code Interrupt */
    /*  - Enable Error Interrupt */
    BXCAN->IER |= CANARD_STM32_CAN_IER_EWGIE
                  | CANARD_STM32_CAN_IER_EPVIE
                  | CANARD_STM32_CAN_IER_BOFIE
                  | CANARD_STM32_CAN_IER_LECIE
                  | CANARD_STM32_CAN_IER_ERRIE;
    BXCAN->IER |= CANARD_STM32_CAN_IER_FMPIE0;
}

void canardSTM32ReleaseFIFO() {
    static volatile uint32_t* const RFxR[2] =
        {
            &BXCAN->RF0R,
            &BXCAN->RF1R
        };

    /*
     * This function must be polled periodically, so we use this opportunity to do it.
     */
    processErrorStatus();

    for (uint_fast8_t i = 0; i < 2; i++)
    {
        if (((*RFxR[i]) & CANARD_STM32_CAN_RFR_FMP_MASK) != 0)
        {
            if (*RFxR[i] & CANARD_STM32_CAN_RFR_FOVR)
            {
                g_stats.rx_overflow_count++;
            }

            // Release FIFO entry we just read
            *RFxR[i] = CANARD_STM32_CAN_RFR_RFOM | CANARD_STM32_CAN_RFR_FOVR | CANARD_STM32_CAN_RFR_FULL;
        }
    }
}

void canardSTM32EnablePeripheral() {
    NVIC_EnableIRQ(CAN_IRQ);
}

void canardSTM32DisablePeripheral() {
    NVIC_DisableIRQ(CAN_IRQ);
}


int canardSTM32Receive(CanardCANFrame* const out_frame)
{
    if (out_frame == NULL)
    {
        return -CANARD_ERROR_INVALID_ARGUMENT;
    }

    static volatile uint32_t* const RFxR[2] =
    {
        &BXCAN->RF0R,
        &BXCAN->RF1R
    };

    /*
     * This function must be polled periodically, so we use this opportunity to do it.
     */
    processErrorStatus();

    /*
     * Reading the TX FIFO
     */
    for (uint_fast8_t i = 0; i < 2; i++)
    {
        volatile CanardSTM32RxMailboxType* const mb = &BXCAN->RxMailbox[i];

        if (((*RFxR[i]) & CANARD_STM32_CAN_RFR_FMP_MASK) != 0)
        {
            if (*RFxR[i] & CANARD_STM32_CAN_RFR_FOVR)
            {
                g_stats.rx_overflow_count++;
            }

            out_frame->id = convertFrameIDRegisterToCanard(mb->RIR);

            out_frame->data_len = (uint8_t)(mb->RDTR & CANARD_STM32_CAN_RDTR_DLC_MASK);

            // Caching to regular (non volatile) memory for faster reads
            const uint32_t rdlr = mb->RDLR;
            const uint32_t rdhr = mb->RDHR;

            out_frame->data[0] = (uint8_t)(0xFF & (rdlr >>  0));
            out_frame->data[1] = (uint8_t)(0xFF & (rdlr >>  8));
            out_frame->data[2] = (uint8_t)(0xFF & (rdlr >> 16));
            out_frame->data[3] = (uint8_t)(0xFF & (rdlr >> 24));
            out_frame->data[4] = (uint8_t)(0xFF & (rdhr >>  0));
            out_frame->data[5] = (uint8_t)(0xFF & (rdhr >>  8));
            out_frame->data[6] = (uint8_t)(0xFF & (rdhr >> 16));
            out_frame->data[7] = (uint8_t)(0xFF & (rdhr >> 24));

            // Release FIFO entry we just read
            *RFxR[i] = CANARD_STM32_CAN_RFR_RFOM | CANARD_STM32_CAN_RFR_FOVR | CANARD_STM32_CAN_RFR_FULL;

            // Reading successful
            return 1;
        }
    }

    // No frames to read
    return 0;
}


int canardSTM32ConfigureAcceptanceFilters(const CanardSTM32AcceptanceFilterConfiguration* const filter_configs,
                                          const unsigned num_filter_configs)
{
    // Note that num_filter_configs = 0 is a valid configuration, which rejects all frames.
    if ((filter_configs == NULL) ||
        (num_filter_configs > CANARD_STM32_NUM_ACCEPTANCE_FILTERS))
    {
        return -CANARD_ERROR_INVALID_ARGUMENT;
    }

    /*
     * First we disable all filters. This may cause momentary RX frame losses, but the application
     * should be able to tolerate that.
     */
    CANARD_STM32_CAN1->FA1R = 0;

    /*
     * Having filters disabled we can update the configuration.
     * Register mapping: FR1 - ID, FR2 - Mask
     */
    for (unsigned i = 0; i < num_filter_configs; i++)
    {
        /*
         * Converting the ID and the Mask into the representation that can be chewed by the hardware.
         * If Mask asks us to accept both STDID and EXTID, we need to use EXT mode on the filter,
         * otherwise it will reject all EXTID frames. This logic is not documented in the RM.
         * True story. This is sacred knowledge! Don't tell the unworthy.
         */
        uint32_t id   = 0;
        uint32_t mask = 0;

        const CanardSTM32AcceptanceFilterConfiguration* const cfg = filter_configs + i;

        if ((cfg->id & CANARD_CAN_FRAME_EFF) || !(cfg->mask & CANARD_CAN_FRAME_EFF))
        {
            // The application wants to trick us by requesting both ext and std frames!
            id   = (cfg->id   & CANARD_CAN_EXT_ID_MASK) << 3;   // Little they know, we can handle that correctly.
            mask = (cfg->mask & CANARD_CAN_EXT_ID_MASK) << 3;   // See?
            id |= CANARD_STM32_CAN_RIR_IDE;                     // That's how we do this. Where's my sunglasses?
        }
        else
        {
            id   = (cfg->id   & CANARD_CAN_STD_ID_MASK) << 21;  // Regular std frames, nothing fancy.
            mask = (cfg->mask & CANARD_CAN_STD_ID_MASK) << 21;  // Boring.
        }

        if (cfg->id & CANARD_CAN_FRAME_RTR)
        {
            id |= CANARD_STM32_CAN_RIR_RTR;
        }

        if (cfg->mask & CANARD_CAN_FRAME_EFF)
        {
            mask |= CANARD_STM32_CAN_RIR_IDE;
        }

        if (cfg->mask & CANARD_CAN_FRAME_RTR)
        {
            mask |= CANARD_STM32_CAN_RIR_RTR;
        }

        /*
         * Applying the converted representation to the registers.
         */
        const unsigned filter_index =
#if CANARD_STM32_USE_CAN2
            i + CANARD_STM32_NUM_ACCEPTANCE_FILTERS;
#else
            i;
#endif
        CANARD_STM32_CAN1->FilterRegister[filter_index].FR1 = id;
        CANARD_STM32_CAN1->FilterRegister[filter_index].FR2 = mask;

        CANARD_STM32_CAN1->FA1R |= 1U << filter_index;      // Enable
    }

    return 0;
}


CanardSTM32Stats canardSTM32GetStats(void)
{
    return g_stats;
}
