// *=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=* 
// ** Copyright UCAR (c) 1990 - 2016                                         
// ** University Corporation for Atmospheric Research (UCAR)                 
// ** National Center for Atmospheric Research (NCAR)                        
// ** Boulder, Colorado, USA                                                 
// ** BSD licence applies - redistribution and use in source and binary      
// ** forms, with or without modification, are permitted provided that       
// ** the following conditions are met:                                      
// ** 1) If the software is modified to produce derivative works,            
// ** such modified software should be clearly marked, so as not             
// ** to confuse it with the version available from UCAR.                    
// ** 2) Redistributions of source code must retain the above copyright      
// ** notice, this list of conditions and the following disclaimer.          
// ** 3) Redistributions in binary form must reproduce the above copyright   
// ** notice, this list of conditions and the following disclaimer in the    
// ** documentation and/or other materials provided with the distribution.   
// ** 4) Neither the name of UCAR nor the names of its contributors,         
// ** if any, may be used to endorse or promote products derived from        
// ** this software without specific prior written permission.               
// ** DISCLAIMER: THIS SOFTWARE IS PROVIDED "AS IS" AND WITHOUT ANY EXPRESS  
// ** OR IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED      
// ** WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.    
// *=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=*=* 
///////////////////////////////////////////////////////////////
// StatsMgr.cc
//
// Mike Dixon, RAP, NCAR, P.O.Box 3000, Boulder, CO, 80307-3000, USA
//
// Aug 2006
//
///////////////////////////////////////////////////////////////
//
// StatsMgr manages the stats, and prints out
//
////////////////////////////////////////////////////////////////

#include "StatsMgr.hh"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <cerrno>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctime>
#include <toolsa/DateTime.hh>
#include <toolsa/uusleep.h>
#include <toolsa/toolsa_macros.h>
#include <toolsa/pmu.h>
#include <toolsa/file_io.h>
#include <toolsa/mem.h>
#include <toolsa/TaXml.hh>
#include <radar/RadarComplex.hh>
#include <Spdb/DsSpdb.hh>

using namespace std;

// Constructor

StatsMgr::StatsMgr(const string &prog_name,
		   const Params &params) :
  _progName(prog_name),
  _params(params)
  
{

  _startTime = 0;
  _endTime = 0;
  _startTime360 = 0;
  _prt = 0;
  _el = 0;
  _az = 0;
  _prevAz = -999;
  _azMoved = 0;
  _nRotations = 0;

  _globalCountZdrm = 0;
  _globalSumZdrm = 0;
  _globalSumSqZdrm = 0;
  _globalMeanZdrm = -9999;
  _globalSdevZdrm = -9999;
  _globalMeanOfSdevZdrm = -9999;

  // set up layers
  
  _nLayers = _params.n_layers;
  _startHt = _params.start_height;
  _deltaHt = _params.delta_height;

  for (int ii = 0; ii < _nLayers; ii++) {
    double minHt = _startHt + (ii - 0.5) * _deltaHt;
    double maxHt = minHt + _deltaHt;
    LayerStats *layer = new LayerStats(_params, minHt, maxHt);
    _layers.push_back(layer);
    _maxHt = maxHt;    
  }
  
}

// destructor

StatsMgr::~StatsMgr()

{

  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    delete _layers[ii];
  }

}

////////////////////
// set the azimuth

void StatsMgr::setAz(double az) {

  _az = az;

  if (_prevAz < -900) {
    _prevAz = _az;
  } else {
    double azDiff = RadarComplex::diffDeg(_prevAz, _az);
    _azMoved += fabs(azDiff);
    _prevAz = _az;
  }
  
}
 
/////////////////////////////////
// check and compute when ready

void StatsMgr::checkCompute() {

  if (_azMoved > 360.0) {

    if (_startTime360 == 0.0) {
      _startTime360 = _startTime;
    }

    computeStats360();
    writeResults360();
    if (_params.write_results_to_spdb) {
      writeResults360ToSpdb();
    }
    clearStats360();
    _azMoved = 0;
    _nRotations++;
    _startTime360 = _endTime;

    if (_nRotations % _params.nrevs_for_global_stats == 0) {
      computeGlobalStats();
      writeGlobalResults();
    }

  }
  
}
 
/////////////////////////////////
// add data to layer

void StatsMgr::addLayerData(double range,
			    const MomentData &mdata)

{
  
  int layer = (int) ((range - _startHt) / _deltaHt);
  if (layer >= 0 && layer < _nLayers) {
    _layers[layer]->addData(mdata);
  }
  
}
 
////////////////////////////////////////////
// clear stats info

void StatsMgr::clearStats360()

{
  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    _layers[ii]->clearData();
  }
}
  
/////////////////////////
// compute stats for 360

void StatsMgr::computeStats360()
  
{
  
  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    _layers[ii]->computeStats();
    if (_params.debug >= Params::DEBUG_VERBOSE) {
      _layers[ii]->print(cerr);
    }
  }

  // compute Zdr for this rotation
  // and accumulate stats for computing mean Zdr

  double sumValid = 0.0;
  double sumZdrm = 0.0;
  double sum2Zdrm = 0.0;

  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    const LayerStats &layer = *(_layers[ii]);
    double ht = layer.getMeanHt();
    double snr = layer.getMean().snr;
    if (ht >= _params.min_ht_for_stats &&
	ht <= _params.max_ht_for_stats &&
        snr >= _params.min_snr) {
      sumValid += layer.getNValid();
      sumZdrm += layer.getSum().zdrm;
      sum2Zdrm += layer.getSum2().zdrm;
    }
  }

  _countZdrm = sumValid;
  _meanZdrm = -9999;
  _sdevZdrm = -9999;

  if (_countZdrm > 0) {
    _meanZdrm = sumZdrm / _countZdrm;
    _globalCountZdrm++;
    _globalSumZdrm += _meanZdrm;
    _globalSumSqZdrm += _meanZdrm * _meanZdrm;
  }
  if (_countZdrm > 2) {
    _meanZdrm = sumZdrm / _countZdrm;
    double variance =
      (sum2Zdrm - (sumZdrm * sumZdrm) / _countZdrm) / (_countZdrm - 1.0);
    _sdevZdrm = 0.000001;
    if (variance >= 0.0) {
      _sdevZdrm = sqrt(variance);
    }
  }

}

/////////////////////////
// compute global stats

void StatsMgr::computeGlobalStats()
  
{
  
  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    _layers[ii]->computeGlobalStats();
  }

  // compute global Zdrm stats

  if (_globalCountZdrm > 0) {
    _globalMeanZdrm = _globalSumZdrm / _globalCountZdrm;
  }
  if (_globalCountZdrm > 2) {
    _globalMeanZdrm = _globalSumZdrm / _globalCountZdrm;
    double variance =
      (_globalSumSqZdrm - (_globalSumZdrm * _globalSumZdrm) / 
       _globalCountZdrm) / (_globalCountZdrm - 1.0);
    _globalSdevZdrm = 0.000001;
    if (variance >= 0.0) {
      _globalSdevZdrm = sqrt(variance);
    }
    _globalMeanOfSdevZdrm = _globalSdevZdrm / sqrt(_globalCountZdrm);
  }

}

//////////////////////////////////////
// write out 360 deg results to files

int StatsMgr::writeResults360()

{

  // create the directory for the output files, if needed

  if (ta_makedir_recurse(_params.output_dir)) {
    int errNum = errno;
    cerr << "ERROR - StatsMgr::_writeResults";
    cerr << "  Cannot create output dir: " << _params.output_dir << endl;
    cerr << "  " << strerror(errNum) << endl;
    return -1;
  }
  
  // compute output file path

  time_t fileTime = (time_t) _startTime;
  DateTime ftime(fileTime);
  char outPath[1024];
  sprintf(outPath, "%s/vert_zdr_cal_%.4d%.2d%.2d_%.2d%.2d%.2d.txt",
          _params.output_dir,
          ftime.getYear(),
          ftime.getMonth(),
          ftime.getDay(),
          ftime.getHour(),
          ftime.getMin(),
          ftime.getSec());

  // open file
  
  FILE *out;
  if ((out = fopen(outPath, "w")) == NULL) {
    int errNum = errno;
    cerr << "ERROR - StatsMgr::_writeFile";
    cerr << "  Cannot create file: " << outPath << endl;
    cerr << "  " << strerror(errNum) << endl;
    return -1;
  }

  printResults360(out);
  if (_params.debug) {
    printResults360(stderr);
  }

  if (_params.debug) {
    cerr << "-->> Writing 360 results file: " << outPath << endl;
  }

  // close file

  fclose(out);
  return 0;

}

///////////////////////////////
// print out results for 360

void StatsMgr::printResults360(FILE *out)

{

  // check we have some valid results to print

  bool resultsFound = false;
  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    const LayerStats &layer = *(_layers[ii]);
    if (layer.getMean().snr > -9990) {
      resultsFound = true;
      break;
    }
  }
  if (!resultsFound) {
    return;
  }
  
  time_t fileTime = (time_t) _startTime;
  
  fprintf(out, "========================================\n");
  fprintf(out, "Vertical-pointing ZDR calibration\n");
  fprintf(out, "  Time: %s\n", DateTime::strm(fileTime).c_str());
  fprintf(out, "  n samples             : %d\n", _params.n_samples);
  fprintf(out, "  min snr (dB)          : %g\n", _params.min_snr);
  fprintf(out, "  max snr (dB)          : %g\n", _params.max_snr);
  fprintf(out, "  min vel (m/s)         : %g\n", _params.min_vel);
  fprintf(out, "  max vel (m/s)         : %g\n", _params.max_vel);
  fprintf(out, "  min rhohv             : %g\n", _params.min_rhohv);
  fprintf(out, "  max ldr               : %g\n", _params.max_ldr);
  fprintf(out, "  zdr_n_sdev            : %g\n", _params.zdr_n_sdev);
  fprintf(out, "  min ht for stats (km) : %g\n", _params.min_ht_for_stats);
  fprintf(out, "  max ht for stats (km) : %g\n", _params.max_ht_for_stats);
  fprintf(out, "  mean ZDRm (dB)        : %g\n", _meanZdrm);
  fprintf(out, "  sdev ZDRm (dB)        : %g\n", _sdevZdrm);
  fprintf(out, "  n for sdev ZDRm stats : %g\n", _countZdrm);
  
  fprintf(out, "========================================\n");
  fprintf(out, "%10s%10s%10s%10s%10s%10s%10s%10s%10s\n",
          "Ht", "npts", "snr", "dBZ", "vel", "zdr_m",
          "ldrh", "ldrv", "rhohv");
  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    const LayerStats &layer = *(_layers[ii]);
    if (layer.getMean().snr > -9990) {
      fprintf(out,
              "%10.2f%10d%10.3f%10.3f%10.1f%10.3f%10.3f%10.3f%10.3f\n",
              layer.getMeanHt(),
              layer.getNValid(),
              layer.getMean().snr,
              layer.getMean().dbz,
              layer.getMean().vel,
              layer.getMean().zdrm,
              layer.getMean().ldrh,
              layer.getMean().ldrv,
              layer.getMean().rhohv);
    }
  }
  
}

///////////////////////////////
// write results to SPDB

int StatsMgr::writeResults360ToSpdb()

{

  // check we have some valid results to print

  bool resultsFound = false;
  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    const LayerStats &layer = *(_layers[ii]);
    if (layer.getMean().snr > -9990) {
      resultsFound = true;
      break;
    }
  }
  if (!resultsFound) {
    return 0;
  }

  // create XML string

  string xml;

  xml += TaXml::writeStartTag("VertPointingResults", 0);

  xml += TaXml::writeDouble("meanZdrm", 1, _meanZdrm);
  xml += TaXml::writeDouble("sdevZdrm", 1, _sdevZdrm);
  xml += TaXml::writeDouble("countZdrm", 1, _countZdrm);

  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    const LayerStats &layer = *(_layers[ii]);
    if (layer.getMean().snr > -9990) {

      xml += TaXml::writeStartTag("LayerStats", 1);
      xml += TaXml::writeDouble("meanHt", 2, layer.getMeanHt());
      xml += TaXml::writeInt("nValid", 2, layer.getNValid());
      xml += TaXml::writeDouble("meanSnr", 2, layer.getMean().snr);
      xml += TaXml::writeDouble("meanDbz", 2, layer.getMean().dbz);
      xml += TaXml::writeDouble("meanVel", 2, layer.getMean().vel);
      xml += TaXml::writeDouble("meanZdrm", 2, layer.getMean().zdrm);
      xml += TaXml::writeDouble("meanLdrh", 2, layer.getMean().ldrh);
      xml += TaXml::writeDouble("meanLdrv", 2, layer.getMean().ldrv);
      xml += TaXml::writeDouble("meanRhohv", 2, layer.getMean().rhohv);
      xml += TaXml::writeEndTag("LayerStats", 1);

    }
  }
  
  xml += TaXml::writeEndTag("VertPointingResults", 0);
  
  if (_params.debug >= Params::DEBUG_VERBOSE) {
    cerr << "Writing XML results to SPDB:" << endl;
    cerr << xml << endl;
  }

  DsSpdb spdb;
  time_t validTime = (time_t) _startTime360;
  si32 dataType = Spdb::hash4CharsToInt32(_params.radar_name);
  spdb.addPutChunk(dataType, validTime, validTime, xml.size() + 1, xml.c_str());
  if (spdb.put(_params.spdb_output_url,
               SPDB_XML_ID, SPDB_XML_LABEL)) {
    cerr << "ERROR - StatsMgr::writeResults360ToSpdb" << endl;
    cerr << spdb.getErrStr() << endl;
    return -1;
  }
  
  if (_params.debug) {
    cerr << "Wrote results to spdb, url: " << _params.spdb_output_url << endl;
    cerr << "  Valid time: " << DateTime::strm(validTime) << endl;
  }

  return 0;

}

///////////////////////////////
// write out results to files

int StatsMgr::writeGlobalResults()

{
  
  // compute output file path

  time_t startTime = (time_t) _startTime;
  DateTime ftime(startTime);
  char outPath[1024];
  sprintf(outPath, "%s/vert_zdr_global_cal_%.4d%.2d%.2d_%.2d%.2d%.2d.txt",
          _params.output_dir,
          ftime.getYear(),
          ftime.getMonth(),
          ftime.getDay(),
          ftime.getHour(),
          ftime.getMin(),
          ftime.getSec());
  
  // open file
  
  FILE *out;
  if ((out = fopen(outPath, "w")) == NULL) {
    int errNum = errno;
    cerr << "ERROR - StatsMgr::_writeFile";
    cerr << "  Cannot create file: " << outPath << endl;
    cerr << "  " << strerror(errNum) << endl;
    return -1;
  }

  // print to file

  printGlobalResults(out);
  if (_params.debug) {
    printGlobalResults(stderr);
  }

  if (_params.debug) {
    cerr << "-->> Writing global results file: " << outPath << endl;
  }

  // close file

  fclose(out);
  return 0;

}

///////////////////////////////
// print global results

void StatsMgr::printGlobalResults(FILE *out)

{
  
  time_t startTime = (time_t) _startTime;
  time_t endTime = (time_t) _endTime;

  fprintf(out, "========================================\n");
  fprintf(out, "Vertical-pointing ZDR calibration - global\n");
  fprintf(out, "Start time: %s\n", DateTime::strm(startTime).c_str());
  fprintf(out, "End time  : %s\n", DateTime::strm(endTime).c_str());
  fprintf(out, "  n samples             : %d\n", _params.n_samples);
  fprintf(out, "  n complete rotations  : %d\n", _nRotations);
  fprintf(out, "  min snr (dB)          : %g\n", _params.min_snr);
  fprintf(out, "  max snr (dB)          : %g\n", _params.max_snr);
  fprintf(out, "  min vel (m/s)         : %g\n", _params.min_vel);
  fprintf(out, "  max vel (m/s)         : %g\n", _params.max_vel);
  fprintf(out, "  min rhohv             : %g\n", _params.min_rhohv);
  fprintf(out, "  max ldr               : %g\n", _params.max_ldr);
  fprintf(out, "  zdr_n_sdev            : %g\n", _params.zdr_n_sdev);
  fprintf(out, "  min ht for stats (km) : %g\n", _params.min_ht_for_stats);
  fprintf(out, "  max ht for stats (km) : %g\n", _params.max_ht_for_stats);
  fprintf(out, "  mean ZDRm (dB)        : %g\n", _globalMeanZdrm);
  fprintf(out, "  sdev ZDRm (dB)        : %g\n", _globalSdevZdrm);
  fprintf(out, "  sdev of mean ZDRm (dB): %g\n", _globalMeanOfSdevZdrm);
  fprintf(out, "  n for sdev ZDRm stats : %g\n", _globalCountZdrm);

  fprintf(out, "========================================\n");
  fprintf(out, "%10s%10s%10s%10s%10s%10s%10s%10s%10s\n",
          "Ht", "npts", "snr", "dBZ", "vel", "zdr_m",
          "ldrh", "ldrv", "rhohv");
  for (int ii = 0; ii < (int) _layers.size(); ii++) {
    const LayerStats &layer = *(_layers[ii]);
    if (layer.getMean().snr > -9990) {
      fprintf(out,
              "%10.2f%10d%10.3f%10.3f%10.1f%10.3f%10.3f%10.3f%10.3f\n",
              layer.getMeanHt(),
              layer.getGlobalNValid(),
              layer.getGlobalMean().snr,
              layer.getGlobalMean().dbz,
              layer.getGlobalMean().vel,
              layer.getGlobalMean().zdrm,
              layer.getGlobalMean().ldrh,
              layer.getGlobalMean().ldrv,
              layer.getGlobalMean().rhohv);
    }
  }

}

