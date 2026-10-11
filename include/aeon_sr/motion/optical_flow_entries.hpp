#pragma once

#include <cstdint>

namespace aeon_sr {
namespace flow_entries {

constexpr uint32_t kCount = 18;
constexpr uint32_t kFirstModel = 10;

constexpr const char *kNames[kCount] = {
	"CSLuma", "CSDownsample", "CSCoarseTop", "CSCoarse", "CSMedian", "CSRefine",
	"CSGlobal", "CSConfidence", "CSExport", "CSCopyFlow",
	"CSStructure", "CSModelTerms", "CSModelReduce", "CSModelSolve", "CSDecision", "CSFuse", "CSPhotoTerms",
	"CSThetaPublish",
};

constexpr const char *kQualityDefine[2] = { "1", "2" };

}
}
