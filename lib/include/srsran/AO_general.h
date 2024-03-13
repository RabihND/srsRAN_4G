#ifndef AO_GENERAL_H
#define AO_GENERAL_H

#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>

//#define CQI_LOG_FILE "/tmp/cqi_log.csv"
#define RNTI_TO_TMSI_FILE_NAME "/tmp/rnti_to_tmsi.csv"
#define TMSI_TO_IMSI_FILE_NAME "/tmp/tmsi_to_imsi.csv"
#define PUSCH_SNR_FILE "/tmp/pusch_snr_rnti.csv"
#define PUCCH_SNR_FILE "/tmp/pucch_snr_rnti.csv"
#define WB_CQI_FILE      "/tmp/wb_cqi.csv"
//#define SB_CQI_LOG      "SB_CQI.csv"
#define SB_CQI_FILE    "/tmp/sb_cqi.csv"
#define PMI_LOG_FILE "/tmp/pmi.csv"
#define RI_LOG_FILE "/tmp/ri.csv"
#define STDOUT_PUSCH_SNR "/tmp/stdout_pusch.csv"
#define RRC_INFO_RESP_FILE "/tmp/rrc_info_resp.csv"
#define OLD_NEW_RNTI_FILE "/tmp/old_new_rnti.csv"

// Funcitons
class AO_LogsHelper
{
    private:
    
    void static createLookupFile(char const *fileName,char const *headerText){
        std::ofstream file;
        file.open(fileName, std::ios_base::trunc | std::ios_base::in); 
        file << headerText << std::endl;
        file.close();
    };

    
    public:
    
    static double time_stamp_double(){
        auto tp = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(tp).count() * 1e-3;
    }

    std::string static time_stamp(){
        std::stringstream ss;
        double time_stamp_double;

        // Number of nanoseconds between the epoch (01-Jan-1970 00:00:00) and the current UTC time
        // Note that this is implementation dependent, it could be the number of microseconds or the maximum resolution available.
        const std::chrono::time_point<std::chrono::high_resolution_clock> now = std::chrono::high_resolution_clock::now();

        // Number of seconds since the epoch, used to display the standard local time
        std::time_t nofSeconds = std::chrono::high_resolution_clock::to_time_t(now);

        // Calculate the microseconds portion of the time
        typedef std::chrono::duration<int, std::ratio_multiply<std::chrono::hours::period, std::ratio<24>>::type> Days;
        auto duration = now.time_since_epoch();
        time_stamp_double = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count() * 1e-3;

        Days days = std::chrono::duration_cast<Days>(duration);
        duration -= days;
        auto hours = std::chrono::duration_cast<std::chrono::hours>(duration);
        duration -= hours;
        auto minutes = std::chrono::duration_cast<std::chrono::minutes>(duration);
        duration -= minutes;
        auto seconds = std::chrono::duration_cast<std::chrono::seconds>(duration);
        duration -= seconds;
        auto microseconds = std::chrono::duration_cast<std::chrono::microseconds>(duration);

        // What remains in duration is the nanoseconds if available by the compiler
        duration -= microseconds; 

        // Create a string of the formatted local time up to seconds precision. Use the same srsRAN time stamp format:
        // yyyy-mm-ddThh:mm:ss
        struct tm * timeinfo;
        char buffer [80];
        time ( &nofSeconds );
        timeinfo = localtime ( &nofSeconds );
        strftime (buffer,80,"%Y-%m-%dT%H:%M:%S",timeinfo);
    
        // Add the microseconds
        ss << buffer << "." << std::setfill('0') << std::setw(6) << microseconds.count();
        ss << "," << std::fixed << std::setprecision(3) << time_stamp_double;
        return ss.str();
    }

    void static create_log_files()
    {
        create_rnti_to_tmsi_file();
        create_pusch_snr_file();
        create_pucch_snr_file();
        create_wb_cqi_file();
        create_sb_cqi_file();
        create_pmi_file();
        create_ri_file();
        create_stdout_pusch_file();
        create_rrc_info_resp_file();
        create_old_new_rnti_file();
    }

    void static open_lookup_file(std::ofstream& log_file, char const *file_name){
        
        log_file.open(file_name, std::ios_base::app | std::ios_base::in); // opens the file
        
        if(!log_file){
            std::cerr << "Error: file could not be opened " << file_name << std::endl;
            exit(1);
        }
    };

    void static close_lookup_file(std::ofstream& log_file){
        log_file.close();
    }
        
    void static add_lookup_line(std::ofstream &log_file, uint32_t _key, uint32_t _value)
    {
        log_file << "0x" << std::hex << _key << ",0x" << _value << std::endl;
    }

    void static add_lookup_line(std::ofstream &log_file, uint32_t _key, std::string _value)
    {
        log_file << "0x" << std::hex << _key << std::dec << "," << _value << std::endl;
    }

    void static add_time_stamp_line(std::ofstream &log_file, std::string str)
    {
        log_file << time_stamp() << ',' << str << std::endl;
    }

    void static create_old_new_rnti_file()
    {
        createLookupFile(OLD_NEW_RNTI_FILE,"old_rnti, new_rnti");
    }
    void static create_rnti_to_tmsi_file(){
        createLookupFile(RNTI_TO_TMSI_FILE_NAME,"rnti,tmsi,event");
    };
    void static create_tmsi_to_imsi_file(){
        createLookupFile(TMSI_TO_IMSI_FILE_NAME,"tmsi,imsi");
    };
    void static create_pusch_snr_file()
    {        
        createLookupFile(PUSCH_SNR_FILE,"time stamp,time stamp double,rnti,snr,crc,epre_dbfs,evm,nof bits, nof re, nof bitsE");
    };
    void static create_pucch_snr_file()
    {        
        createLookupFile(PUCCH_SNR_FILE,"time stamp,time stamp double,rnti,snr,detected");
    };
    void static create_wb_cqi_file()
    {        
        createLookupFile(WB_CQI_FILE,"time stamp,time stamp double,rnti,scell_index,wb_cqi");
    };
    void static create_sb_cqi_file()
    {        
        createLookupFile(SB_CQI_FILE,"time stamp,time stamp double,rnti,scell_index,sb_index,sb_cqi");
    };
    void static create_pmi_file()
    {        
        createLookupFile(PMI_LOG_FILE,"time stamp,time stamp double,rnti,scell_index,pmi");
    };
    void static create_ri_file()
    {        
        createLookupFile(RI_LOG_FILE,"time stamp,time stamp double,rnti,scell_index,ri");
    };
    void static create_stdout_pusch_file()
    {
        createLookupFile(STDOUT_PUSCH_SNR,"time stamp,time stamp double,rnti,snr,data bits, nof tti, bit rate");
    }
    void static create_rrc_info_resp_file()
    {
        createLookupFile(RRC_INFO_RESP_FILE,"time stamp,time stamp double,rnti,rsrp,rsrq");
    }


};


#endif // AO_GENERAL_H