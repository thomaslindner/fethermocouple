//
// feRTD.cxx
//
// Frontend for Phidget 4x RTD
//

#include <stdio.h>
#include <signal.h> // SIGPIPE
#include <assert.h> // assert()
#include <stdlib.h> // malloc()
#include <sstream>
#include <iomanip>

#include "midas.h"
#include "tmfe.h"
#include "Thermocouple.hh"

#define NMAX 6

void callback(INT hDB, INT hkey, INT index, void *feptr);

class Myfe :
   public TMFeRpcHandlerInterface,
   public  TMFePeriodicHandlerInterface
{
public:
   TMFE* fMfe;
   TMFeEquipment* fEq;

   int fEventSize;
   char* fEventBuf;


   std::vector<Thermocouple*> rtd;
   std::vector<int> hubPorts = std::vector<int>(NMAX, -1);
   std::vector<std::string> type = std::vector<std::string>(NMAX, "RTD_TYPE_PT100_3850");
   std::vector<double> temperature = std::vector<double>(NMAX, 0.);

   int serNum = 0;

   Myfe(TMFE* mfe, TMFeEquipment* eq) // ctor
   {
      fMfe = mfe;
      fEq  = eq;      
      fEventSize = 0;
      fEventBuf  = NULL;
   }

   ~Myfe() // dtor
   {
      for(Thermocouple* r: rtd){
         delete r;
      }
      rtd.clear();
      fMfe->Disconnect();

      if (fEventBuf) {
         free(fEventBuf);
         fEventBuf = NULL;
      }
   }

   void Init()
   {

      fEventSize = 100;
      if (fEventBuf) {
         free(fEventBuf);
      }
      fEventBuf = (char*)malloc(fEventSize);

      fEq->fOdbEqSettings->RIA("HubPorts", &hubPorts, true, NMAX);
      fEq->fOdbEqSettings->RI("SerNo", &serNum, true);
      fEq->fOdbEqSettings->RSA("Type", &type, true, NMAX);
      fEq->fOdbEqVariables->RDA("Temperature", &temperature, true, NMAX);
      temperature = std::vector<double>(NMAX, 0.);
      fEq->fOdbEqVariables->WDA("Temperature", temperature);

      bool success = CreatePhidgets();
      if(!success) std::cout << "Failed to create phidget " << std::endl;
      if(success) success = InitPhidgets();

      if(!success){
         for(Thermocouple *r: rtd){
            if(!r->AllGood()){
               fMfe->Msg(MERROR, "Init", "Phidget connection failed! Err: %s", r->GetErrorCode().c_str());
            }
         }
         exit(13);
      }
      char tmpbuf[80];
      sprintf(tmpbuf, "/Equipment/%s/Settings", fMfe->fFrontendName.c_str());
      HNDLE hkey;
      db_find_key(fMfe->fDB, 0, tmpbuf, &hkey);
      db_watch(fMfe->fDB, hkey, callback, (void*)this);
      std::cout << "Init done." << std::endl;
   }

   /** \brief Function called on ODB setting change.*/
   void fecallback(HNDLE hDB, HNDLE hkey, INT index)
   {
      std::vector<int> hubPorts_old = hubPorts;
      fEq->fOdbEqSettings->RIA("HubPorts", &hubPorts);
      fEq->fOdbEqSettings->RI("SerNo", &serNum);
      fEq->fOdbEqSettings->RSA("Type", &type);
      bool success = true;
      if(hubPorts != hubPorts_old)
         success = CreatePhidgets();
      if(success)
         success = InitPhidgets();
      if(!success){
         for(Thermocouple *r: rtd){
            if(!r->AllGood()){
               fMfe->Msg(MERROR, "Init", "Phidget connection failed! Err: %s", r->GetErrorCode().c_str());
            }
         }
         exit(13);
      }
      std::cout << "fecallback done." << std::endl;
   }

   std::string HandleRpc(const char* cmd, const char* args)
   {
      fMfe->Msg(MINFO, "HandleRpc", "RPC cmd [%s], args [%s]", cmd, args);
      return "OK";
   }

   void HandleBeginRun()
   {
      fMfe->Msg(MINFO, "HandleBeginRun", "Begin run!");
      fEq->SetStatus("Running", "#00FF00");
   }

   void HandleEndRun()
   {
      fMfe->Msg(MINFO, "HandleEndRun", "End run!");
      fEq->SetStatus("Stopped", "#00FF00");
   }

   void HandlePeriodic()
   {
      // std::cout << "Periodic" << std::endl;
      auto it = rtd.begin();
      for(unsigned int i = 0; i < hubPorts.size(); i++){
         if(hubPorts[i] >= 0){
            temperature[i] = (*it)->GetTemp();
            it++;
         }
      }
      fEq->fOdbEqVariables->WDA("Temperature", temperature);
      fEq->SetStatus(StatString().c_str(), "lightgreen");

      
      // Send event with thermocouple data
      fEq->ComposeEvent(fEventBuf, fEventSize);
      fEq->BkInit(fEventBuf, fEventSize);
      
      double* ptr = (double*)fEq->BkOpen(fEventBuf, "TMP0", TID_DOUBLE);
      for(int i = 0; i < 6; i++){
         *ptr++ = temperature[i];
      }
      fEq->BkClose(fEventBuf, ptr+1);

      fEq->SendEvent(fEventBuf);

      fEq->WriteStatistics();
   }


   void SendEvent(double dvalue)
   {
      fEq->ComposeEvent(fEventBuf, fEventSize);
      fEq->BkInit(fEventBuf, fEventSize);
         
      double* ptr = (double*)fEq->BkOpen(fEventBuf, "test", TID_DOUBLE);
      *ptr = dvalue;
      fEq->BkClose(fEventBuf, ptr+1);

      std::cout << "Send event?" << std::endl;
      fEq->SendEvent(fEventBuf);
   }

   std::string StatString(){
      std::ostringstream oss;
      oss << std::setprecision(1) << std::fixed;
      oss << "Running:";
      for(unsigned int i = 0; i < hubPorts.size(); i++){
         if(hubPorts[i] >= 0){
            static bool first = true;
            oss << (first?"":"\t") << temperature[i] << "°C";
            first = false;
         }
      }
      return oss.str();
   }

private:
   bool CreatePhidgets()
   {
      for(Thermocouple* r: rtd){
         delete r;
      }
      rtd.clear();
      bool success = true;
      for(unsigned int i = 0; i < hubPorts.size(); i++){
         if(hubPorts[i] >= 0){
            std::cout << "Creating Phidget " << i << " " << " " << hubPorts[i] << serNum << std::endl;
            rtd.push_back(new Thermocouple(hubPorts[i], serNum, false));
            std::cout << "Finished constructor " << std::endl;
            success &= rtd.back()->AllGood();
            if(success)
               std::cout << "Phidget " << i << " created" << std::endl;
         }
      }
      return success;
   }
   bool InitPhidgets()
   {
      bool success = true;
      auto it = rtd.begin();
      for(unsigned int i = 0; i < hubPorts.size(); i++){
         if(hubPorts[i] >= 0){
            PhidgetTemperatureSensor_ThermocoupleType rtdtype(THERMOCOUPLE_TYPE_K);
            if(type.back() == std::string("THERMOCOUPLE_TYPE_J")){
               rtdtype = THERMOCOUPLE_TYPE_J;
            } else if(type.back() == std::string("THERMOCOUPLE_TYPE_K")){
               rtdtype = THERMOCOUPLE_TYPE_K;
            } else if(type.back() == std::string("THERMOCOUPLE_TYPE_E")){
               rtdtype = THERMOCOUPLE_TYPE_E;
            } else if(type.back() == std::string("THERMOCOUPLE_TYPE_T")){
               rtdtype = THERMOCOUPLE_TYPE_T;
            }
            (*it)->SetType(rtdtype);
            success &= (*it)->AllGood();
            if(success)
               std::cout << "Phidget " << i << " initialized" << std::endl;
            it++;
         }
      }
      return success;
   }

};

/** \brief global wrapper for Midas callback of class function
 *
 */
void callback(INT hDB, INT hkey, INT index, void *feptr)
{
   Myfe* fe = (Myfe*)feptr;
   fe->fecallback(hDB, hkey, index);
}

static void usage()
{
   fprintf(stderr, "Usage: fethermocouple <name> ...\n");
   exit(1);
}

int main(int argc, char* argv[])
{
   // setbuf(stdout, NULL);
   // setbuf(stderr, NULL);

   signal(SIGPIPE, SIG_IGN);

   std::string name = "";

   if (argc == 2) {
      name = argv[1];
   } else {
      usage(); // DOES NOT RETURN
   }

   TMFE* mfe = TMFE::Instance();

   TMFeError err = mfe->Connect("fethermocouple", __FILE__);
   if (err.error) {
      printf("Cannot connect, bye.\n");
      return 1;
   }

   //mfe->SetWatchdogSec(0);

   TMFeCommon *common = new TMFeCommon();
   common->EventID = 1;
   common->LogHistory = 1;
   //common->Buffer = "SYSTEM";

   TMFeEquipment* eq = new TMFeEquipment(mfe, "fethermocouple", common);
   eq->Init();
   eq->SetStatus("Starting...", "white");
   eq->ZeroStatistics();
   eq->WriteStatistics();

   mfe->RegisterEquipment(eq);

   Myfe* myfe = new Myfe(mfe, eq);

   mfe->RegisterRpcHandler(myfe);

   //mfe->SetTransitionSequenceStart(910);
   //mfe->SetTransitionSequenceStop(90);
   //mfe->DeregisterTransitionPause();
   //mfe->DeregisterTransitionResume();

   myfe->Init();

   mfe->RegisterPeriodicHandler(eq, myfe);

   eq->SetStatus(myfe->StatString().c_str(), "lightgreen");

   while (!mfe->fShutdownRequested) {
      mfe->PollMidas(10);
   }

   mfe->Disconnect();

   return 0;
}

/* emacs
 * Local Variables:
 * tab-width: 8
 * c-basic-offset: 3
 * indent-tabs-mode: nil
 * End:
 */
