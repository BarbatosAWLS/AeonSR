#pragma once

#include <cstdint>

namespace aeon_sr {
namespace flow_entries {

constexpr uint32_t kCount = 22;
constexpr uint32_t kFirstModel = 11;

constexpr const char *kNames[kCount] = {
	"CSLuma", "CSDownsample", "CSCoarseTop", "CSCoarse", "CSMedian", "CSRefine",
	"CSGlobal", "CSConfidence", "CSExport", "CSCopyFlow", "CSSeed",
	"CSStructure", "CSModelTerms", "CSModelReduce", "CSModelSolve", "CSDecision", "CSFuse", "CSPhotoTerms",
	"CSThetaPublish", "CSDfmEpipole", "CSDfmMeasure", "CSDfmWarp",
};

constexpr uint32_t kTermsBlock = 32;

constexpr const char *kQualityDefine[2] = { "1", "2" };

}
}
