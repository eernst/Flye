#include "haplotype_resolver.h"
#include "graph_processing.h"

//This function collapses simply bubbles caused by
//alternative haplotypes / strains. They are defined as follows:
//1. Structure: 1 input, 2 branches, 1 output: -<>-
//2. Size of each branch is shorter than MAX_BUBBLE_LEN below
//3. Total coverage of bubbles branches roughly equasl to input/output coverages
//4. Each branch is shorter than both entrace and exits. We need this to
//   distinguish from the case of two repeats of multiplicity 2
//Note that we are not using any global coverage assumptions here.
int HaplotypeResolver::collapseHeterozygousBulges(bool removeAlternatives)
{
	const float MAX_COV_VAR = 1.5;
	const int MAX_BUBBLE_LEN = Config::get("max_bubble_length");

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<FastaRecord::Id> toSeparate;
	int numMasked = 0;
	for (auto& path : unbranchingPaths)
	{
		if (path.isLooped()) continue;

		std::vector<UnbranchingPath*> twoPaths;
		for (auto& candEdge : unbranchingPaths)
		{
			if (candEdge.nodeLeft() == path.nodeLeft() &&
				candEdge.nodeRight() == path.nodeRight()) 
			{
				twoPaths.push_back(&candEdge);
			}
		}

		//making sure the structure is ok
		if (twoPaths.size() != 2) continue;
		if (twoPaths[0]->id == twoPaths[1]->id.rc()) continue;
		if (toSeparate.count(twoPaths[0]->id) || 
			toSeparate.count(twoPaths[1]->id)) continue;
		if (twoPaths[0]->nodeLeft()->inEdges.size() != 1 ||
			twoPaths[0]->nodeLeft()->outEdges.size() != 2 ||
			twoPaths[0]->nodeRight()->outEdges.size() != 1 ||
			twoPaths[0]->nodeRight()->inEdges.size() != 2) continue;

		UnbranchingPath* entrancePath = nullptr;
		UnbranchingPath* exitPath = nullptr;
		for (auto& cand : unbranchingPaths)
		{
			if (cand.nodeRight() == 
				twoPaths[0]->nodeLeft()) entrancePath = &cand;
			if (cand.nodeLeft() == twoPaths[0]->nodeRight()) exitPath = &cand;
		}

		//sanity check for maximum bubble size
		if (std::max(twoPaths[0]->length, twoPaths[1]->length) > 
			MAX_BUBBLE_LEN) continue;

		//coverage requirement: sum over two branches roughly equals to
		//exit and entrance coverage or less
		float covSum = twoPaths[0]->meanCoverage + twoPaths[1]->meanCoverage;
		if (covSum > std::min(entrancePath->meanCoverage * MAX_COV_VAR,
							  exitPath->meanCoverage * MAX_COV_VAR)) continue;

		//require bubble branches to be shorter than entrance or exit,
		//to distinguish from the case of two consecutive repeats
		//of multiplicity 2
		if (std::max(twoPaths[0]->length, twoPaths[1]->length) >
			std::max(entrancePath->length, exitPath->length)) continue;

		if (twoPaths[0]->meanCoverage > twoPaths[1]->meanCoverage)
		{
			std::swap(twoPaths[0], twoPaths[1]);
		}

		if (!twoPaths[0]->path.front()->altHaplotype ||
			!twoPaths[1]->path.front()->altHaplotype) ++numMasked;

		for (size_t i = 0; i < 2; ++i)
		{
			for (auto& edge : twoPaths[i]->path)
			{
				edge->altHaplotype = true;
				_graph.complementEdge(edge)->altHaplotype = true;
			}
		}

		if (removeAlternatives)
		{
			toSeparate.insert(twoPaths[0]->id);
			toSeparate.insert(twoPaths[0]->id.rc());
			for (auto& edge : twoPaths[1]->path)
			{
				edge->meanCoverage += twoPaths[0]->meanCoverage;
				_graph.complementEdge(edge)->meanCoverage += twoPaths[0]->meanCoverage;
				edge->altHaplotype = false;
				_graph.complementEdge(edge)->altHaplotype = false;
			}
		}
	}

	if (removeAlternatives)
	{
		for (auto& path : unbranchingPaths)
		{
			if (toSeparate.count(path.id))
			{
				//Logger::get().debug() << "Seperated branch: " << path.edgesStr();

				GraphNode* newLeft = _graph.addNode();
				GraphNode* newRight = _graph.addNode();
				vecRemove(path.nodeLeft()->outEdges, path.path.front());
				vecRemove(path.nodeRight()->inEdges, path.path.back());
				path.nodeLeft() = newLeft;
				path.nodeRight() = newRight;
				newLeft->outEdges.push_back(path.path.front());
				newRight->inEdges.push_back(path.path.back());
			}
		}

		Logger::get().debug() << "[SIMPL] Removed " << toSeparate.size() / 2 
			<< " heterozygous bulges";

		_aligner.updateAlignments();
		return toSeparate.size() / 2;
	}
	else
	{
		Logger::get().debug() << "[SIMPL] Masked " << numMasked
			<< " heterozygous bulges";
		return numMasked;
	}
}

//This function collapses simple loops:
//1. One loop edge with one entrance and one exit
//2. Loop length is shorter than lengths of entrance/exit
//3. Loop coverage is roughly equal or less than coverage of entrance/exit
int HaplotypeResolver::collapseHeterozygousLoops(bool removeAlternatives)
{
	const float COV_MULT = 1.5;

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();

	std::unordered_set<FastaRecord::Id> toUnroll;
	std::unordered_set<FastaRecord::Id> toRemove;
	int numMasked = 0;
	for (auto& loop : unbranchingPaths)
	{
		if (!loop.id.strand()) continue;
		if (!loop.isLooped()) continue;
		if (loop.path.front()->selfComplement) continue;

		GraphNode* node = loop.nodeLeft();
		if (node->inEdges.size() != 2 ||
			node->outEdges.size() != 2) continue;

		UnbranchingPath* entrancePath = nullptr;
		UnbranchingPath* exitPath = nullptr;
		for (auto& cand : unbranchingPaths)
		{
			if (cand.nodeRight() == node &&
				loop.id != cand.id) entrancePath = &cand;
			if (cand.nodeLeft() == node &&
				loop.id != cand.id) exitPath = &cand;
		}

		if (entrancePath->isLooped()) continue;
		if (entrancePath->id == exitPath->id.rc()) continue;

		//loop coverage should be roughly equal or less
		if (loop.meanCoverage > 
				COV_MULT * std::min(entrancePath->meanCoverage, 
									entrancePath->meanCoverage)) continue;

		//loop should not be longer than other branches
		if (loop.length > std::max(entrancePath->length, 
								   exitPath->length)) continue;

		if (!loop.path.front()->altHaplotype) ++numMasked;
		for (auto& edge : loop.path)
		{
			edge->altHaplotype = true;
			_graph.complementEdge(edge)->altHaplotype = true;
		}
		//either remove or unroll loop, depending on the coverage
		if (loop.meanCoverage < 
			(entrancePath->meanCoverage + exitPath->meanCoverage) / 4)
		{
			toRemove.insert(loop.id);
			toRemove.insert(loop.id.rc());
		}
		else
		{
			toUnroll.insert(loop.id);
			toUnroll.insert(loop.id.rc());
		}
	}

	if (removeAlternatives)
	{
		for (auto& path : unbranchingPaths)
		{
			if (toUnroll.count(path.id))
			{
				//Logger::get().debug() << "Unrolled loop: " << path.edgesStr();

				GraphNode* newNode = _graph.addNode();
				size_t id = path.nodeLeft()->inEdges[0] == path.path.back();
				GraphEdge* prevEdge = path.nodeLeft()->inEdges[id];

				vecRemove(path.nodeLeft()->outEdges, path.path.front());
				vecRemove(path.nodeLeft()->inEdges, prevEdge);
				path.nodeLeft() = newNode;
				newNode->outEdges.push_back(path.path.front());
				prevEdge->nodeRight = newNode;
				newNode->inEdges.push_back(prevEdge);
			}
			if (toRemove.count(path.id))
			{
				//Logger::get().debug() << "Removed loop: " << path.edgesStr();

				GraphNode* newLeft = _graph.addNode();
				GraphNode* newRight = _graph.addNode();

				vecRemove(path.nodeLeft()->outEdges, path.path.front());
				vecRemove(path.nodeLeft()->inEdges, path.path.back());
				path.nodeLeft() = newLeft;
				newRight->inEdges.push_back(path.path.back());
				path.nodeRight() = newRight;
				newLeft->outEdges.push_back(path.path.front());
			}
		}

		Logger::get().debug() << "[SIMPL] Removed " << (toRemove.size() + toUnroll.size()) / 2
			<< " heterozygous loops";
		_aligner.updateAlignments();
		return (toRemove.size() + toUnroll.size()) / 2;
	}
	else
	{
		Logger::get().debug() << "[SIMPL] Masked " << numMasked << " heterozygous loops";
		return numMasked;
	}
}

//this function reveals complex heterogenities on the graph
//(more than just two alternative branches) using read-paths
int HaplotypeResolver::findComplexHaplotypes()
{
	std::unordered_map<GraphEdge*, 
					   std::vector<GraphAlignment>> alnIndex;
	for (auto& aln : _aligner.getAlignments())
	{
		if (aln.size() > 1)
		{
			std::unordered_set<GraphEdge*> uniqueEdges;
			for (auto& edgeAln : aln)
			{
				uniqueEdges.insert(edgeAln.edge);
			}
			for (GraphEdge* edge : uniqueEdges) alnIndex[edge].push_back(aln);
		}
	}

	GraphProcessor proc(_graph, _asmSeqs);
	auto unbranchingPaths = proc.getUnbranchingPaths();
	std::unordered_set<GraphEdge*> loopedEdges;
	for (auto& path : unbranchingPaths)
	{
		if (path.isLooped())
		{
			loopedEdges.insert(path.path.begin(), path.path.end());
		}
	}

	for (auto& startPath: unbranchingPaths)
	{
		if (!startPath.id.strand()) continue;
		if (startPath.nodeRight()->outEdges.size() < 2) continue;

		GraphEdge* startEdge = startPath.path.back();
		if (loopedEdges.count(startEdge)) continue;

		//first, extract alnignment paths starting from
		//the current edge and sort them from longest to shortest
		std::vector<GraphAlignment> outPaths;
		for (auto& aln : alnIndex[startEdge])
		{
			for (size_t i = 0; i < aln.size(); ++i)
			{
				if (aln[i].edge == startEdge)
				{
					outPaths.emplace_back(GraphAlignment(aln.begin() + i, 
														 aln.end()));
					break;
				}
			}
		}
		if (outPaths.empty()) continue;
		std::sort(outPaths.begin(), outPaths.end(),
				  [](GraphAlignment& a1, GraphAlignment& a2)
				  {return a1.back().overlap.curEnd - a1.front().overlap.curEnd >
				    	  a2.back().overlap.curEnd - a2.front().overlap.curEnd;});

		//now group the path by containmnent. For each group we'll have 
		//a longest "reference" path.
		struct PathWithScore
		{
			GraphAlignment path;
			int score;
		};
		const int MIN_SCORE = std::max(2UL, outPaths.size() / 10);
		std::vector<PathWithScore> pathGroups;
		for (auto& trgPath: outPaths)
		{
			bool newPath = true;
			for (auto& referencePath : pathGroups)
			{
				bool contained = true;
				for (size_t i = 0; i < std::min(trgPath.size(), 
												referencePath.path.size()); ++i)
				{
					if (trgPath[i].edge != referencePath.path[i].edge)
					{
						contained = false;
						break;
					}
				}
				if (contained)
				{
					newPath = false;
					++referencePath.score;
					break;
				}
			}
			if (newPath)
			{
				pathGroups.push_back({trgPath, 1});
			}
		}
		pathGroups.erase(std::remove_if(pathGroups.begin(), pathGroups.end(),
				 	   	 [MIN_SCORE](PathWithScore& p)
					     {return p.score < MIN_SCORE;}), pathGroups.end());
		if (pathGroups.size() < 2) continue;

		//mark edges that appear more than once as repeats
		std::unordered_set<GraphEdge*> repeats;
		for (size_t groupId = 0; groupId < pathGroups.size(); ++groupId)
		{
			std::unordered_set<GraphEdge*> seen;
			for (size_t i = 0; i < pathGroups[groupId].path.size(); ++i)
			{
				if (seen.count(pathGroups[groupId].path[i].edge))
				{
					repeats.insert(pathGroups[groupId].path[i].edge);
				}
				seen.insert(pathGroups[groupId].path[i].edge);
			}
		}

		//now, set the longest path as reference, and find
		//edges where other groups coverge with the reference
		PathWithScore& refPath = pathGroups.front();
		std::unordered_set<GraphEdge*> convergenceEdges;
		for (size_t i = 0; i < refPath.path.size(); ++i)
		{
			if (!loopedEdges.count(refPath.path[i].edge) &&
				!repeats.count(refPath.path[i].edge))
			{
				convergenceEdges.insert(refPath.path[i].edge);
			}
		}
		for (size_t groupId = 1; groupId < pathGroups.size(); ++groupId)
		{
			std::unordered_set<GraphEdge*> newSet;
			for (size_t i = 0; i < pathGroups[groupId].path.size(); ++i)
			{
				if (convergenceEdges.count(pathGroups[groupId].path[i].edge))
				{
					newSet.insert(pathGroups[groupId].path[i].edge);
				}
			}
			convergenceEdges = newSet;
		}

		//get the bubble start (paths might be convergent for a bit)
		size_t bubbleStartId = 0;
		for (;;)
		{
			bool agreement = true;
			for (size_t groupId = 1; groupId < pathGroups.size(); ++groupId)
			{
				if (bubbleStartId + 1 >= pathGroups[groupId].path.size() ||
					!convergenceEdges.count(pathGroups[0].path[bubbleStartId + 1].edge) ||
						(pathGroups[groupId].path[bubbleStartId + 1].edge !=
						 pathGroups[0].path[bubbleStartId + 1].edge))
				{
					agreement = false;
					break;
				}
			}
			if (!agreement) break;
			++bubbleStartId;
		}

		//get the bubble end
		bool foundEnd = false;
		size_t bubbleEndId = bubbleStartId + 1;
		for (; bubbleEndId < refPath.path.size(); ++bubbleEndId)
		{
			if (convergenceEdges.count(refPath.path[bubbleEndId].edge))
			{
				foundEnd = true;
				break;
			}
		}
		if (!foundEnd) continue;

		//shorten all branches accordingly
		std::vector<PathWithScore> bubbleBranches;
		for (size_t groupId = 0; groupId < pathGroups.size(); ++groupId)
		{
			size_t groupStart = 0;
			size_t groupEnd = 0;
			for (size_t i = 0; i < pathGroups[groupId].path.size(); ++i)
			{
				if (pathGroups[groupId].path[i].edge == 
					refPath.path[bubbleStartId].edge) groupStart = i;

				if (pathGroups[groupId].path[i].edge == 
					refPath.path[bubbleEndId].edge) groupEnd = i;
			}
			GraphAlignment newPath(pathGroups[groupId].path.begin() + groupStart,
							  	   pathGroups[groupId].path.begin() + groupEnd + 1);
			PathWithScore newBranch = {newPath, pathGroups[groupId].score};

			bool duplicate = false;
			for (size_t branchId = 0; branchId < bubbleBranches.size(); ++branchId)
			{
				if (newBranch.path.size() != 
					bubbleBranches[branchId].path.size()) continue;
				if (std::equal(newBranch.path.begin(), newBranch.path.end(),
							   bubbleBranches[branchId].path.begin(),
							   [](EdgeAlignment& a1, EdgeAlignment& a2)
							   {return a1.edge == a2.edge;}))
				{
					duplicate = true;
					bubbleBranches[branchId].score += newBranch.score;
				}
			}
			if (!duplicate) bubbleBranches.push_back(newBranch);
		}
		if (bubbleBranches.size() < 2) continue;

		Logger::get().debug() << "Haplo paths " 
			<< startEdge->edgeId.signedId() << " " << outPaths.size();
		/*for (auto& aln : outPaths)
		{
			std::string pathStr;
			for (size_t i = 0; i < aln.size(); ++i)
			{
				pathStr += std::to_string(aln[i].edge->edgeId.signedId()) + " -> ";
			}
			Logger::get().debug() << "\tPath: " << pathStr;
		}*/
		for (auto& aln : pathGroups)
		{
			std::string pathStr;
			for (size_t i = 0; i < aln.path.size(); ++i)
			{
				pathStr += std::to_string(aln.path[i].edge->edgeId.signedId()) + " -> ";
			}
			Logger::get().debug() << "\tGroup: " << pathStr << aln.score;
		}
		for (auto& aln : bubbleBranches)
		{
			std::string pathStr;
			for (size_t i = 0; i < aln.path.size(); ++i)
			{
				pathStr += std::to_string(aln.path[i].edge->edgeId.signedId()) + " -> ";
			}
			Logger::get().debug() << "\tBranch: " << pathStr << aln.score;
		}

		Logger::get().debug() << "Boundaries: " << refPath.path[bubbleStartId].edge->edgeId.signedId()
			<< " -> " << refPath.path[bubbleEndId].edge->edgeId.signedId();
	}
	return 0;
}
