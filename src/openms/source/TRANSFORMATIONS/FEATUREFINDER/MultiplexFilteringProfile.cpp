// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2016.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Lars Nilse $
// $Authors: Lars Nilse $
// --------------------------------------------------------------------------

#include <OpenMS/KERNEL/StandardTypes.h>
#include <OpenMS/KERNEL/BaseFeature.h>
#include <OpenMS/CONCEPT/Constants.h>
#include <OpenMS/CHEMISTRY/IsotopeDistribution.h>
#include <OpenMS/TRANSFORMATIONS/RAW2PEAK/PeakPickerHiRes.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/MultiplexFilteringProfile.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/MultiplexIsotopicPeakPattern.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/MultiplexFilteredMSExperiment.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/MultiplexFilterResult.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/MultiplexFilterResultRaw.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/MultiplexFilterResultPeak.h>
#include <OpenMS/TRANSFORMATIONS/FEATUREFINDER/MultiplexSatelliteCentroided.h>
#include <OpenMS/FILTERING/DATAREDUCTION/SplineSpectrum.h>
#include <OpenMS/MATH/STATISTICS/StatisticFunctions.h>
#include <OpenMS/MATH/MISC/CubicSpline2d.h>

#include <vector>
#include <queue>
#include <algorithm>
#include <iostream>
#include <QDir>

using namespace std;
using namespace boost::math;

namespace OpenMS
{

  MultiplexFilteringProfile::MultiplexFilteringProfile(MSExperiment& exp_profile, const MSExperiment& exp_picked, const std::vector<std::vector<PeakPickerHiRes::PeakBoundary> >& boundaries, const std::vector<MultiplexIsotopicPeakPattern> patterns, int isotopes_per_peptide_min, int isotopes_per_peptide_max, double intensity_cutoff, double rt_band, double mz_tolerance, bool mz_tolerance_unit, double peptide_similarity, double averagine_similarity, double averagine_similarity_scaling, String averagine_type) :
    MultiplexFiltering(exp_picked, patterns, isotopes_per_peptide_min, isotopes_per_peptide_max, intensity_cutoff, rt_band, mz_tolerance, mz_tolerance_unit, peptide_similarity, averagine_similarity, averagine_similarity_scaling, averagine_type), boundaries_(boundaries)
  {
    
    if (exp_profile.size() != exp_picked.size())
    {
      stringstream stream;
      stream << "Profile and centroided data do not contain same number of spectra. (";
      stream << exp_profile.size();
      stream << "!=";
      stream << exp_picked.size();
      stream << ")";
      throw Exception::IllegalArgument(__FILE__, __LINE__, OPENMS_PRETTY_FUNCTION, stream.str());
    }

    if (exp_picked.size() != boundaries.size())
    {
      stringstream stream;
      stream << "Centroided data and the corresponding list of peak boundaries do not contain same number of spectra. (";
      stream << exp_picked.size();
      stream << "!=";
      stream << boundaries.size();
      stream << ")";
      throw Exception::IllegalArgument(__FILE__, __LINE__, OPENMS_PRETTY_FUNCTION, stream.str());
    }
    
    // spline interpolate the profile data
    for (MSExperiment::Iterator it = exp_profile.begin(); it < exp_profile.end(); ++it)
    {
      exp_spline_profile_.push_back(SplineSpectrum(*it));
    }
    
    // TODO: Constructing the navigators here instead in the beginning of the filter() method results in segmentation faults. Why?

  }
  
  vector<MultiplexFilteredMSExperiment> MultiplexFilteringProfile::filter()
  {
    // progress logger
    unsigned progress = 0;
    startProgress(0, patterns_.size() * exp_spline_profile_.size(), "filtering LC-MS data");

    // list of filter results for each peak pattern
    std::vector<MultiplexFilteredMSExperiment> filter_results;
    
    std::cout << "\nStart filtering.\n\n";
      
    unsigned int start = clock();
    
    // construct navigators for all spline spectra
    std::vector<SplineSpectrum::Navigator> navigators;
    for (std::vector<SplineSpectrum>::iterator it = exp_spline_profile_.begin(); it < exp_spline_profile_.end(); ++it)
    {
      SplineSpectrum::Navigator nav = (*it).getNavigator();
      navigators.push_back(nav);
    }
    
    // loop over all patterns
    //for (unsigned pattern_idx = 0; pattern_idx < patterns_.size(); ++pattern_idx)
    // DEBUG: for now only first pattern
    for (unsigned pattern_idx = 0; pattern_idx < 1; ++pattern_idx)
    {
      std::cout << "\npattern = " << pattern_idx << "\n";
      
      // current pattern
      MultiplexIsotopicPeakPattern pattern = patterns_[pattern_idx];
      
      // data structure storing peaks which pass all filters
      MultiplexFilteredMSExperiment result;

      // construct new white experiment
      White2Original exp_picked_mapping;
      MSExperiment exp_picked_white = getWhiteMSExperiment_(exp_picked_mapping);

      // loop over spectra
      // loop simultaneously over RT in the spline interpolated profile and (white) centroided experiment (including peak boundaries)
      std::vector<SplineSpectrum>::iterator it_rt_profile;
      MSExperiment::ConstIterator it_rt_picked;
      std::vector<std::vector<PeakPickerHiRes::PeakBoundary> >::const_iterator it_rt_boundaries;
      for (it_rt_profile = exp_spline_profile_.begin(), it_rt_picked = exp_picked_white.begin(), it_rt_boundaries = boundaries_.begin();
           it_rt_profile < exp_spline_profile_.end() && it_rt_picked < exp_picked_white.end() && it_rt_boundaries < boundaries_.end();
           ++it_rt_profile, ++it_rt_picked, ++it_rt_boundaries)
      {
        // skip empty spectra
        if ((*it_rt_profile).size() == 0 || (*it_rt_picked).size() == 0 || (*it_rt_boundaries).size() == 0)
        {
          continue;
        }
        
        setProgress(++progress);
        
        double rt = it_rt_picked->getRT();
        MSExperiment::ConstIterator it_rt_picked_band_begin = exp_picked_white.RTBegin(rt - rt_band_/2);
        MSExperiment::ConstIterator it_rt_picked_band_end = exp_picked_white.RTEnd(rt + rt_band_/2);
        
        std::cout << "    RT = " << rt << "\n";
        
        // loop over mz
        for (MSSpectrum<Peak1D>::ConstIterator it_mz = it_rt_picked->begin(); it_mz != it_rt_picked->end(); ++it_mz)
        {
          double mz = it_mz->getMZ();
          MultiplexFilteredPeak peak(mz, rt, exp_picked_mapping[it_rt_picked - exp_picked_white.begin()][it_mz - it_rt_picked->begin()], it_rt_picked - exp_picked_white.begin());
          
          //std::cout << "        mz = " << mz << "     mz idx (white) = " << (it_mz - it_rt_picked->begin()) << "     mz idx (original) = " << exp_picked_mapping[it_rt_picked - exp_picked_white.begin()][it_mz - it_rt_picked->begin()] << "\n";
          
          if (!(filterPeakPositions_(it_mz, exp_picked_mapping, exp_picked_white.begin(), it_rt_picked_band_begin, it_rt_picked_band_end, pattern, peak)))
          {
            continue;
          }
          
          size_t mz_idx = exp_picked_mapping[it_rt_picked - exp_picked_white.begin()][it_mz - it_rt_picked->begin()];
          double peak_min = (*it_rt_boundaries)[mz_idx].mz_min;
          double peak_max = (*it_rt_boundaries)[mz_idx].mz_max;
          
          double rt_peak = peak.getRT();
          double mz_peak = peak.getMZ();

          std::cout << "        mz = " << mz << " (" << peak_min << ", " << peak_max << ")\n";
          
          std::multimap<size_t, MultiplexSatelliteCentroided > satellites = peak.getSatellites();
          
          // Arrangement of peaks looks promising. Now scan through the spline fitted profile data around the peak i.e. from peak boundary to peak boundary.
          for (double mz_profile = peak_min; mz_profile < peak_max; mz_profile = navigators[it_rt_profile - exp_spline_profile_.begin()].getNextMz(mz_profile))
          {
            // determine m/z shift relative to the centroided peak at which the profile data will be sampled
            double mz_shift = mz_profile - mz_peak;

            // construct the set of spline-interpolated satellites for this specific mz_profile
            std::multimap<size_t, MultiplexSatelliteProfile > satellites_profile;
            for (std::multimap<size_t, MultiplexSatelliteCentroided >::const_iterator satellite_it = satellites.begin(); satellite_it != satellites.end(); ++satellite_it)
            {
              // find indices of the peak
              size_t rt_idx = (satellite_it->second).getRTidx();
              size_t mz_idx = (satellite_it->second).getMZidx();
              
              // find peak itself
              MSExperiment::ConstIterator it_rt = exp_picked_.begin();
              std::advance(it_rt, rt_idx);
              MSSpectrum<Peak1D>::ConstIterator it_mz = it_rt->begin();
              std::advance(it_mz, mz_idx);
              
              double rt_satellite = it_rt->getRT();
              double mz_satellite = it_mz->getMZ();
              
              // determine m/z and corresponding intensity
              double mz = mz_satellite + mz_shift;
              double intensity = navigators[rt_idx].eval(mz);
              
              MultiplexSatelliteProfile s(rt_satellite, mz, intensity);
              satellites_profile.insert(std::make_pair(satellite_it->first, s));
            }
            
            bool x = filterAveragineModel_(pattern, navigators, peak, mz_profile);
          }
          
        }
      }
      
      // add results of this pattern to list
      filter_results.push_back(result);
    }
        
    std::cout << "\nThat took me " << (float)(clock()-start)/CLOCKS_PER_SEC << " seconds.\n";
    std::cout << "\nFinished filtering.\n\n";

    endProgress();

    return filter_results;
  }

  bool MultiplexFilteringProfile::filterAveragineModel_(const MultiplexIsotopicPeakPattern& pattern, std::vector<SplineSpectrum::Navigator>& navigators, const MultiplexFilteredPeak& peak, double mz_sampling) const
  {
    // construct averagine distribution
    // Note that the peptide(s) are very close in mass. We therefore calculate the averagine distribution only once (for the lightest peptide).
    double mass = peak.getMZ() * pattern.getCharge();
    IsotopeDistribution distribution;
    distribution.setMaxIsotope(isotopes_per_peptide_max_);
    if (averagine_type_ == "peptide")
    {
        distribution.estimateFromPeptideWeight(mass);
    }
    else if (averagine_type_ == "RNA")
    {
        distribution.estimateFromRNAWeight(mass);
    }
    else if (averagine_type_ == "DNA")
    {
        distribution.estimateFromDNAWeight(mass);
    }
    else
    {
        throw Exception::InvalidParameter(__FILE__, __LINE__, OPENMS_PRETTY_FUNCTION, "Invalid averagine type.");
    }   
    
    // debug output variables
    /*int debug_charge = 2;
    size_t debug_rt_idx = 39;
    size_t debug_mz_idx = 130;
    bool debug_now = ((pattern.getCharge() == debug_charge) && (peak.getRTidx() == debug_rt_idx) && (peak.getMZidx() == debug_mz_idx));*/
    
    // debug output
    /*if (debug_now)
    {
      std::cout << "Inside the Averagine Filter.\n";
    }*/
 
  
    // determine m/z shift relative to the centroided peak at which the profile data will be sampled
    double rt_peak = peak.getRT();
    double mz_peak = peak.getMZ();
    double mz_shift = mz_sampling - mz_peak;
    
    // loop over peptides
    for (size_t peptide = 0; peptide < pattern.getMassShiftCount(); ++peptide)
    {
      // intensities for the Pearson and Spearman rank correlations
      std::vector<double> intensities_model;
      std::vector<double> intensities_data;
      
      // loop over isotopes i.e. mass traces of the peptide
      for (size_t isotope = 0; isotope < isotopes_per_peptide_max_; ++isotope)
      {
        size_t idx = peptide * isotopes_per_peptide_max_ + isotope;
        std::pair<std::multimap<size_t, MultiplexSatelliteCentroided >::const_iterator, std::multimap<size_t, MultiplexSatelliteCentroided >::const_iterator> satellites;
        satellites = peak.getSatellites().equal_range(idx);
        
        int count = 0;
        double sum_intensities = 0;
        
        // loop over satellites in mass trace
        for (std::multimap<size_t, MultiplexSatelliteCentroided >::const_iterator satellite_it = satellites.first; satellite_it != satellites.second; ++satellite_it)
        {
          // find indices of the peak
          size_t rt_idx = (satellite_it->second).getRTidx();
          size_t mz_idx = (satellite_it->second).getMZidx();
          
          // find peak itself
          MSExperiment::ConstIterator it_rt = exp_picked_.begin();
          std::advance(it_rt, rt_idx);
          MSSpectrum<Peak1D>::ConstIterator it_mz = it_rt->begin();
          std::advance(it_mz, mz_idx);
          
          double rt_satellite = it_rt->getRT();
          double mz_satellite = it_mz->getMZ();
      
          // determine m/z and corresponding intensity for averagine test
          double mz = mz_satellite + mz_shift;
          double intensity = navigators[rt_idx].eval(mz);
          
          ++count;
          sum_intensities += intensity;
        }
        
        if (count > 0)
        {
          intensities_model.push_back(distribution.getContainer()[isotope].second);
          intensities_data.push_back(sum_intensities/count);
        }

        // debug output
        /*if (debug_now)
        {
          std::cout << "    peptide = " << peptide << "    isotope = " << isotope << "    count = " << count << "    average intensity = " << sum_intensities/count << "    averagine intensity = " << distribution.getContainer()[isotope].second << "\n";
        }*/
        
      }
      
      // Calculate Pearson and Spearman rank correlations
      if ((intensities_model.size() < isotopes_per_peptide_min_) || (intensities_data.size() < isotopes_per_peptide_min_))
      {
        throw Exception::InvalidSize(__FILE__, __LINE__, OPENMS_PRETTY_FUNCTION, 0);
      }
      double correlation_Pearson = OpenMS::Math::pearsonCorrelationCoefficient(intensities_model.begin(), intensities_model.end(), intensities_data.begin(), intensities_data.end());
      double correlation_Spearman = OpenMS::Math::rankCorrelationCoefficient(intensities_model.begin(), intensities_model.end(), intensities_data.begin(), intensities_data.end());

      // debug output
      /*if (debug_now)
      {
        std::cout << "        Pearson correlation = " << correlation_Pearson << "    rank correlation = " << correlation_Spearman << "\n";
      }*/
      
      if ((correlation_Pearson < averagine_similarity_) || (correlation_Spearman < averagine_similarity_))
      {
        return false;
      }
      
    }
    
    return true;
  }
  
}
