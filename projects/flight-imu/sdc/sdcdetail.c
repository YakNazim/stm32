/*! \file sdcdetail.c
 *  SD Card
 */

/*!
 * \defgroup sdcdetail SDC Utilities
 * @{
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "ch.h"
#include "hal.h"
#include "chprintf.h"
#include "chrtclib.h"

#include "ff.h"
#include "psas_rtc.h"

#include "MPU9150.h"
#include "MPL3115A2.h"
#include "ADIS16405.h"

#include "crc_16_reflect.h"

#include "sdcdetail.h"

#include "psas_sdclog.h"

#define         DEBUG_SDCLOG

#ifdef DEBUG_SDCLOG
#include "usbdetail.h"
#define SDCLOGDBG(format, ...) chprintf(getUsbStream(), format, ##__VA_ARGS__ )
#else
#define SDCLOGDBG(...) do{ } while ( false )
#endif

enum WHICH_SENSOR { MPU9150=0, MPL3115A2, ADIS16405, SENSOR_LOG_HALT, SENSOR_LOG_START };

static const    char*           sdc_log_data_file                = "LOGSMALL.bin";
static const    unsigned        sdlog_thread_sleeptime_ms        = 1234;

static  bool                    fs_stop                          = true;

static struct dfstate {
    DWORD               filesize;
    uint32_t            log_sequence;
    uint32_t            write_errors;
    DWORD               fp_index;

    GENERIC_message     log_data;

    FILINFO             DATAFil_info;
    FIL                 DATAFil;

    bool                sd_log_opened;
} datafile_state;


static void sdc_log_data(eventid_t id) {
    static const  int32_t      mpu_downsample  = 30;
    static const  int32_t      mpl_downsample  = 30;
    //static const  int32_t      adis_downsample = 20;

    static  int32_t      mpu_count       = 0; 
    static  int32_t      mpl_count       = 0; 
    //static  int32_t      adis_count      = 0; 

    bool                write_log       = false;
    uint32_t            bw;
    FRESULT             f_ret;
    SDC_ERRORCode       sdc_ret;

    if(id == SENSOR_LOG_START) {
        fs_stop  = false;
        return;
    }

    if(fs_ready && !datafile_state.sd_log_opened ) {
        f_ret = f_stat(sdc_log_data_file, &datafile_state.DATAFil_info);
        if(f_ret) {
            SDCLOGDBG("fail stat on file\r\n");
        }
        SDCLOGDBG("file size of %s is: %d\r\n", sdc_log_data_file, datafile_state.DATAFil_info.fsize);
        // open an existing log file for writing
        f_ret = f_open(&datafile_state.DATAFil, sdc_log_data_file, FA_OPEN_EXISTING | FA_READ | FA_WRITE );
        if(f_ret) { // try again....
            SDCLOGDBG("open existing failed ret: %d\r\n", f_ret);
            chThdSleepMilliseconds(500);
            f_ret = f_open(&datafile_state.DATAFil, sdc_log_data_file, FA_OPEN_EXISTING | FA_READ | FA_WRITE );
        }

        if (f_ret) {
            SDCLOGDBG("failed to open existing %s return %d\r\n",sdc_log_data_file, f_ret);
            // ok...try creating the file
            f_ret = f_open(&datafile_state.DATAFil, sdc_log_data_file, FA_CREATE_ALWAYS | FA_WRITE );
            if(f_ret) {
                // try again
                SDCLOGDBG("open new file ret: %d\r\n", f_ret);
                f_ret = f_open(&datafile_state.DATAFil, sdc_log_data_file, FA_CREATE_ALWAYS | FA_WRITE );
            }
            if (f_ret) {
                datafile_state.sd_log_opened = false;
            } else {
                datafile_state.sd_log_opened = true;
            }
        } else {
            SDCLOGDBG("Opened existing file OK.\r\n");
            /* Seek to end of data if first line is good data */
            sdc_ret = sdc_seek_eod(&datafile_state.DATAFil);
            if(sdc_ret == SDC_OK) {
                SDCLOGDBG("found eod marker. %lu\r\n", sdc_fp_index);
            } else {
                SDCLOGDBG("no eod marker. %lu\r\n", sdc_fp_index);
                sdc_reset_fp_index();
            }
            datafile_state.sd_log_opened = true;
            datafile_state.write_errors  = 0;
        }
    }

    if (fs_ready && datafile_state.sd_log_opened) {
        crc_t          crc16;
        RTCTime        timenow;
        int            rc;

        datafile_state.log_data.mh.index        = datafile_state.log_sequence++;

        // timestamp
        timenow.h12                             = 1;
        rc                                      = psas_rtc_get_unix_time( &RTCD1, &timenow) ;
        if (rc == -1) {
            SDCLOGDBG( "%s: psas_rtc time read errors: %d\r\n",__func__, rc);
        }
        datafile_state.log_data.logtime.tv_time = timenow.tv_time;
        datafile_state.log_data.logtime.tv_msec = timenow.tv_msec;
        psas_rtc_to_psas_ts(&datafile_state.log_data.mh.ts, &timenow);

        //SDCLOGDBG("%d ", id);
        switch(id) {
            case MPU9150:
                if(mpu_count++ > mpu_downsample) {
                    //SDCLOGDBG("u");
                    strncpy(datafile_state.log_data.mh.ID, mpuid, sizeof(datafile_state.log_data.mh.ID));
                    memcpy(&datafile_state.log_data.data, (void*) &mpu9150_current_read, sizeof(MPU9150_read_data) );
                    datafile_state.log_data.mh.data_length = sizeof(MPU9150_read_data);
                    mpu_count = 0;
                    write_log = true;
                }
                break;
            case MPL3115A2:
                if(mpl_count++ > mpl_downsample) {
                    //SDCLOGDBG("l");
                    strncpy(datafile_state.log_data.mh.ID, mplid, sizeof(datafile_state.log_data.mh.ID));
                    memcpy(&datafile_state.log_data.data, (void*) &mpl3115a2_current_read, sizeof(MPL3115A2_read_data) );
                    datafile_state.log_data.mh.data_length = sizeof(MPL3115A2_read_data);
                    mpl_count = 0;
                    write_log = true;
                }            
                break;
            case ADIS16405:
                /*
                 *strncpy(datafile_state.log_data.mh.ID, adisid, sizeof(datafile_state.log_data.mh.ID));
                 *memcpy(&datafile_state.log_data.data, (void*) &adis16405_burst_data, sizeof(ADIS16405_burst_data) );
                 *datafile_state.log_data.mh.data_length = sizeof(ADIS16405_burst_data);
                 */
                break;
            case SENSOR_LOG_HALT:
                f_ret = f_close(&datafile_state.DATAFil);
                if(f_ret) { // try again....
                    SDCLOGDBG("close existing failed ret: %d\r\n", f_ret);
                    chThdSleepMilliseconds(5);
                    f_ret = f_close(&datafile_state.DATAFil);
                }
                datafile_state.sd_log_opened = false;
                fs_stop  = true;
                break;
            default:
                break;
        }

        if(write_log) {
            sdc_ret = sdc_write_log_message(&datafile_state.DATAFil, &datafile_state.log_data, &bw) ;
            if(sdc_ret != SDC_OK ) { ++datafile_state.write_errors; }

            // calc checksum
            crc16                   = crc_init();
            crc16                   = crc_update(crc16, (const unsigned char*) &datafile_state.log_data, sizeof(GENERIC_message));
            crc16                   = crc_finalize(crc16);

            sdc_ret = sdc_write_checksum(&datafile_state.DATAFil, &crc16, &bw) ;
            if(sdc_ret != SDC_OK ) { ++datafile_state.write_errors; SDCLOGDBG("checksum write error %d\r\n", datafile_state.write_errors); }

#ifdef DEBUG_SDCLOG
            if((sdc_fp_index - sdc_fp_index_old) > 100000) {
                if(datafile_state.write_errors !=0) {
                    SDCLOGDBG("E%d", datafile_state.write_errors);
                } else {
                    SDCLOGDBG("x");
                }
                sdc_fp_index_old = sdc_fp_index;
            }
#endif
            write_log = false;
        }

    } else {
        if(datafile_state.sd_log_opened) {
            f_ret = f_close(&datafile_state.DATAFil);       // might be redundant if card removed....\sa f_sync
            SDCLOGDBG( "close file ret: %d\r\n", f_ret);
            datafile_state.sd_log_opened = false;
        }
    }
}

/*! Stack area for the sdlog_thread.  */
WORKING_AREA(wa_sdlog_thread, SDC_THREAD_STACKSIZE_BYTES);

/*! \brief sdlog thread.
 *
 * Test logging to microSD card on e407.
 *
 */
msg_t sdlog_thread(void *p) {
    void * arg __attribute__ ((unused)) = p;
    static const evhandler_t evhndl_sdclog[]  = {
        sdc_log_data,
        sdc_log_data,
        sdc_log_data,
        sdc_log_data,
        sdc_log_data
    };
    struct EventListener     el0, el1, el2, el3, el4;

    chRegSetThreadName("sdlog_thread");

#ifdef DEBUG_SDCLOG
    /*chThdSleepMilliseconds(1000);*/
#endif

    SDCLOGDBG("Start sdlog thread\r\n");

    // init structure
    datafile_state.log_sequence  = 0;
    datafile_state.write_errors  = 0;
    datafile_state.sd_log_opened = false;

    sdc_reset_fp_index();

    sdc_init_eod((uint8_t)0xa5);

    // Assert data is halfword aligned
    if(((sizeof(GENERIC_message)*8) % 16) != 0) {
        SDCLOGDBG("%s: GENERIC message is not halfword aligned.\r\n", __func__);
        return (SDC_ASSERT_ERROR);
    }

    // Assert we will not overflow Payload
    if(  (sizeof(MPU9150_read_data)    > (sizeof(datafile_state.log_data.data)-1)) ||
         (sizeof(MPL3115A2_read_data)  > (sizeof(datafile_state.log_data.data)-1)) ||
         (sizeof(ADIS16405_burst_data) > (sizeof(datafile_state.log_data.data)-1))) {
        SDCLOGDBG("%s: DATA size is too large\r\n");
        return (SDC_ASSERT_ERROR);
    }

    chEvtRegister(&mpl3115a2_data_event        ,   &el0, MPL3115A2);
    chEvtRegister(&adis_data_ready,   &el1, ADIS16405);
    chEvtRegister(&mpu9150_data_event          ,   &el2, MPU9150);
    chEvtRegister(&sdc_halt_event              ,   &el3, SENSOR_LOG_HALT);
    chEvtRegister(&sdc_start_event             ,   &el4, SENSOR_LOG_START);
    while(1) {
        if(!fs_stop) {
            chEvtDispatch(evhndl_sdclog, chEvtWaitOneTimeout(ALL_EVENTS, MS2ST(50)));
        } else {
            chEvtDispatch(evhndl_sdclog, chEvtWaitOneTimeout((1<<SENSOR_LOG_START), MS2ST(50)));
        }
    }
    return -1;
}

//! @}

