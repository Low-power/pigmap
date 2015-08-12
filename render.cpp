// Copyright 2010-2012 Michael J. Nelson
//
// This file is part of pigmap.
//
// pigmap is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// pigmap is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with pigmap.  If not, see <http://www.gnu.org/licenses/>.

#include <memory>
#include <iostream>
#include <assert.h>

#include "render.h"
#include "utils.h"

using namespace std;



int ThreadOutputCache::getIndex(const ZoomTileIdx& zti) const
{
	if (zti.zoom != zoom)
		return -1;
	return zti.y * (1 << zoom) + zti.x;
}




// get topmost y-coord in a column (even if column is out-of-bounds--only looks at top edge of bbox)
int64_t topPixelY(int64_t x, int64_t bboxTop, int B)
{
	if ((x % (4*B)) == 0)
		return ceildiv(bboxTop, 2*B) * 2*B;
	return ceildiv(bboxTop - B, 2*B) * 2*B + B;
}

TileBlockIterator::TileBlockIterator(const TileIdx& ti, const MapParams& mp)
	: current(0,0), mparams(mp), tile(ti), expandedBBox(Pixel(0,0), Pixel(0,0))
{
	expandedBBox = ti.getBBox(mparams);
	expandedBBox.topLeft -= Pixel(2*mparams.B - 1, 2*mparams.B - 1);
	expandedBBox.bottomRight += Pixel(2*mparams.B - 1, 2*mparams.B - 1);

	current.x = ceildiv(expandedBBox.topLeft.x, 2*mparams.B) * 2*mparams.B;
	current.y = topPixelY(current.x, expandedBBox.topLeft.y, mparams.B);
	end = false;
	pos = 0;
	lastTop = 0;
	lastBottom = -1;
	nextN = nextE = nextSE = -1;
}

void TileBlockIterator::advance()
{
	// move down the column
	current.y += 2*mparams.B;
	// our current pos is SE of our next pos
	nextSE = pos;
	// when we reset at the top of a column, we may not get an E neighbor, but we always
	//  gete a N one; so if we have no N neighbor at the moment, we're on the left edge
	if (nextN != -1)
	{
		// if we're not on the left edge, then our N neighbor is our next position's E
		//  neighbor, etc.
		nextE = nextN;  // can't just do nextE++; nextE might have been -1
		nextN++;
		// gotta watch for the bottom, though, where we might have no N neighbor
		if (nextE == lastBottom)
			nextN = -1;
	}
	// advance to next pos
	pos++;

	// if we went off the bottom, we need to reset some stuff
	if (current.y >= expandedBBox.bottomRight.y)
	{
		// move over to the next column
		current.x += 2*mparams.B;
		// ...and we can abort now if we've gone off the right edge; that means we're done
		if (current.x >= expandedBBox.bottomRight.x)
		{
			end = true;
			return;
		}
		// find the top of our new column
		current.y = topPixelY(current.x, expandedBBox.topLeft.y, mparams.B);
		// since we're up at the top, we have no SE neighbor
		nextSE = -1;
		// however, we do have a N neighbor, and if the top of the column to the left
		//  is above us, we have an E as well
		if (topPixelY(current.x - 2*mparams.B, expandedBBox.topLeft.y, mparams.B) < current.y)
		{
			nextE = lastTop;
			nextN = nextE + 1;
		}
		else
		{
			nextE = -1;
			nextN = lastTop;
		}
		// finally, remember this new column top's position, and remember that our previous
		//  position was the old column's bottom
		lastTop = pos;
		lastBottom = pos - 1;
	}
}




PseudocolumnIterator::PseudocolumnIterator(const Pixel& center, const MapParams& mp) : current(0,0,0), mparams(mp)
{
	current = BlockIdx::topBlock(center, mp);
	end = false;
}

void PseudocolumnIterator::advance()
{
	current += BlockIdx(1,-1,-1);
	if (current.y < mparams.minY)
		end = true;
}



// travel down two neighboring pseudocolumns, setting occlusion edges between their nodes
// ...the first pcol must be N, E, or SE of the second one, and the "which" parameter tells
//  which pointer from the first goes to the second--e.g. if which == 4, then the first is
//  N of the second, so its S pointer (#4) should be used, and the second's N pointer
//  (which - 3 == #1) should be used
void buildDependencies(SceneGraph& sg, int pcol1, int pcol2, int which)
{
	int node1 = sg.getTopNode(pcol1), node2 = sg.getTopNode(pcol2);
	if (node1 == -1 || node2 == -1)
		return;

	while (true)
	{
		// if node1 occludes node2, then scan down pcol1 and see if there are any lower
		//  nodes that also occlude it; use the lowest one, then set node1 to the one after it
		if (sg.nodes[node1].bi.occludes(sg.nodes[node2].bi))
		{
			int next1 = sg.nodes[node1].children[0];
			while (next1 != -1 && sg.nodes[next1].bi.occludes(sg.nodes[node2].bi))
			{
				node1 = next1;
				next1 = sg.nodes[node1].children[0];
			}
			sg.nodes[node1].children[which] = node2;
			node1 = next1;
		}

		if (node1 == -1)
			return;

		// ...same thing for the other direction
		if (sg.nodes[node2].bi.occludes(sg.nodes[node1].bi))
		{
			int next2 = sg.nodes[node2].children[0];
			while (next2 != -1 && sg.nodes[next2].bi.occludes(sg.nodes[node1].bi))
			{
				node2 = next2;
				next2 = sg.nodes[node2].children[0];
			}
			sg.nodes[node2].children[which - 3] = node1;
			node2 = next2;
		}

		if (node2 == -1)
			return;
	}
}

struct Block
{
	uint16_t id;
	uint8_t data;
	
	inline Block(uint16_t id_, uint8_t data_): id(id_), data(data_) {}
};

inline Block getNeighbor(ChunkData *chunkdata, RenderJob& rj, const PosChunkIdx& ci, const BlockIdx& bin)
{
	PosChunkIdx cin = bin.getChunkIdx();
	if (cin != ci)
		chunkdata = rj.chunkcache->getData(cin);

	return Block(chunkdata->id(bin), chunkdata->data(bin));
}

inline Block getNeighborUD(ChunkData* chunkdata, const BlockIdx& bin)
{
	if (bin.y >= 0 && bin.y <= 255)
		return Block(chunkdata->id(bin), chunkdata->data(bin));
	return Block(0, 0);
}

inline bool connectFence(RenderJob& rj, const Block& block)
{
	return block.id == 85 || block.id == 188 || block.id == 189 || block.id == 190 || block.id == 191 || block.id == 192 ||
		block.id == 107 || block.id == 183 || block.id == 184 || block.id == 185 || block.id == 186 || block.id == 187 || rj.blockimages.isOpaque(block.id, block.data);
}

inline bool connectNetherFence(RenderJob& rj, const Block& block)
{
	return block.id == 113 || block.id == 107 || rj.blockimages.isOpaque(block.id, block.data);
}

inline bool connectCobblestoneWall(RenderJob& rj, const Block& block)
{
    return block.id == 139 || block.id == 107 || block.id == 120 || (rj.blockimages.isOpaque(block.id, block.data) && block.id != 46 && block.id != 89 && block.id != 54 && block.id != 130 && block.id != 29 && block.id != 33);
}

inline bool connectChorusPlant(RenderJob& rj, const Block& block)
{
	return block.id == 199 || block.id == 200;
}
inline bool connectCobblestoneWallUp(RenderJob& rj, const Block& block)
{
    return block.id != 0 || rj.blockimages.isOpaque(block.id, block.data);
}

inline bool connectPane(RenderJob& rj, const Block& block)
{
    return block.id == 101 || block.id == 102 || block.id == 160 || (rj.blockimages.isOpaque(block.id, block.data) && block.id != 130);
}

// fromside: 0 - NS, 1 - WE
inline bool connectRedstone(RenderJob& rj, const Block& block, uint8_t fromside)
{
    return block.id == 28 || block.id == 55 || block.id == 75 || block.id == 75 || block.id ==146 || block.id ==149 || block.id == 150 || block.id == 152 || ((block.id == 93 || block.id == 94) && block.data % 2 == fromside);
}

inline bool connectTripWire(RenderJob& rj, const Block& block)
{
    return block.id == 131 || block.id == 132;
}

inline int getNeighborAirMask(ChunkData* chunkdata, RenderJob& rj, const PosChunkIdx& ci, const BlockIdx& bi)
{
	Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
	Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
	Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
	Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
	return (blockN.id != 0 ? 0x1 : 0) |
			(blockS.id != 0 ? 0x2 : 0) |
			(blockE.id != 0 ? 0x4 : 0) |
			(blockW.id != 0 ? 0x8 : 0);
}

// given a node that must be drawn, see if we need to do anything special to it--that is, anything that
//  doesn't depend purely on its blockID/blockData
// examples: for nodes with no E/S neighbors, we add a little darkness on the EU/SU edge to indicate drop-off;
//  for chests, we may need to draw half of a double chest instead if there's another chest next door; etc.
void checkSpecial(SceneGraphNode& node, uint16_t blockID, uint8_t blockData, const PosChunkIdx& ci, ChunkData *chunkdata, RenderJob& rj)
{
	const BlockIdx& bi = node.bi;
	
	//if (node.bimgoffset == 8)  // solid water
	if ((blockID == 8 || blockID == 9) && (blockData == 0 || blockData > 7))  // solid water
	{
		// if there's water to the W or S, we don't draw those faces
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		bool waterW = blockW.id == 8 || blockW.id == 9;
		bool waterS = blockS.id == 8 || blockS.id == 9;
		if (waterW && waterS)
			node.bimgoffset += 1;
		else if (waterW)
			node.bimgoffset += 2;
		else if (waterS)
			node.bimgoffset += 3;
	}
	else if (blockID == 79)  // ice
	{
		// if there's ice to the W or S, we don't draw those faces
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));

		bool iceW = blockW.id == 79;
		bool iceS = blockS.id == 79;
		if (iceW && iceS)
			node.bimgoffset += 1;
		else if (iceW)
			node.bimgoffset += 2;
		else if (iceS)
			node.bimgoffset += 3;
	}
	else if (blockID == 175) // double flowers
	{
		Block blockD = getNeighborUD(chunkdata, bi + BlockIdx(0,0,-1));
		if(blockD.id == blockID) // if bottom block is double flower too, then draw double flower top
			node.bimgoffset += 2 * blockD.data + 1;
	}
	else if (blockID == 85 || blockID == 188 || blockID == 189 || blockID == 190 || blockID == 191 || blockID == 192 || blockID == 113)  // fences, nether fence
	{
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		int bits;
		if(blockID == 113) // nether fence
			bits = (connectNetherFence(rj, blockN) ? 0x1 : 0) |
		            (connectNetherFence(rj, blockS) ? 0x2 : 0) |
		            (connectNetherFence(rj, blockE) ? 0x4 : 0) |
		            (connectNetherFence(rj, blockW) ? 0x8 : 0);
		else // fence
			bits = (connectFence(rj, blockN) ? 0x1 : 0) |
		            (connectFence(rj, blockS) ? 0x2 : 0) |
		            (connectFence(rj, blockE) ? 0x4 : 0) |
		            (connectFence(rj, blockW) ? 0x8 : 0);
		if (bits != 0)
			node.bimgoffset += bits;
	}
	else if (blockID == 139) // cobblestone wall
	{
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		Block blockU = getNeighborUD(chunkdata, bi + BlockIdx(0,0,1));
		int bits = (connectCobblestoneWall(rj, blockN) ? 0x1 : 0) |
					(connectCobblestoneWall(rj, blockS) ? 0x2 : 0) |
					(connectCobblestoneWall(rj, blockE) ? 0x4 : 0) |
					(connectCobblestoneWall(rj, blockW) ? 0x8 : 0);
		
		if (bits != 0) 
		{
			if(connectCobblestoneWallUp(rj, blockU))
				node.bimgoffset += bits;
			else if (bits == 3)
				node.bimgoffset += 16;
			else if(bits == 12)
				node.bimgoffset += 17;
			else
				node.bimgoffset += bits;
		}
	}
	else if(blockID == 199) // chorus plant (workaround)
	{
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		int bits = (connectChorusPlant(rj, blockN) ? 0x1 : 0) |
					(connectChorusPlant(rj, blockS) ? 0x2 : 0) |
					(connectChorusPlant(rj, blockE) ? 0x4 : 0) |
					(connectChorusPlant(rj, blockW) ? 0x8 : 0);
					
		if (bits != 0)
		{
			node.bimgoffset += bits;
		}
	}
	else if (blockID == 54 || blockID == 146)  // chest
	{
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		// if there's another chest to the N, make this a southern half
		if (blockN.id == blockID)
			node.bimgoffset += (blockN.data == 4) ? 6 : 10;
		// ...or if there's one to the S, make this a northern half
		else if (blockS.id == blockID)
			node.bimgoffset += (blockS.data == 4) ? 5 : 9;
		// ...same deal with E/W
		else if (blockW.id == blockID)
			node.bimgoffset += (blockW.data == 2) ? 4 : 5;
		else if (blockE.id == blockID)
			node.bimgoffset += (blockE.data == 2) ? 3 : 4;
	}
	else if (blockID == 101 || blockID == 102 || blockID == 160)  // iron bars, glass pane, stained glass pane
	{
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		// decide which edges to draw based on which neighbors are not air (zero neighbors gets the full cross)
		int bits = ((connectPane(rj, blockN)) ? 0x1 : 0) |
		            ((connectPane(rj, blockS)) ? 0x2 : 0) |
		            ((connectPane(rj, blockE)) ? 0x4 : 0) |
		            ((connectPane(rj, blockW)) ? 0x8 : 0);
		if (bits != 0 && bits != 15)
			node.bimgoffset += bits;
	}
	else if (blockID == 55 || blockID == 132)  // wire
	{
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		// decide which edges to draw based on which neighbors are not air (zero neighbors gets EW)
		int bits = 0;
		if(blockID == 55)
			bits = (connectRedstone(rj, blockN, 0) ? 0x1 : 0) |
					(connectRedstone(rj, blockS, 0) ? 0x2 : 0) | 
					(connectRedstone(rj, blockW, 1) ? 0x4 : 0) | 
					(connectRedstone(rj, blockE, 1) ? 0x8 : 0);
		else if(blockID == 132)
			bits = (connectTripWire(rj, blockN) ? 0x1 : 0) | 
					(connectTripWire(rj, blockS) ? 0x2 : 0) | 
					(connectTripWire(rj, blockW) ? 0x4 : 0) | 
					(connectTripWire(rj, blockE) ? 0x8 : 0);
		else
			bits = (blockN.id == blockID ? 0x1 : 0) | 
					(blockS.id == blockID ? 0x2 : 0) | 
					(blockW.id == blockID ? 0x4 : 0) | 
					(blockE.id == blockID ? 0x8 : 0);
		static const int wireOffsets[16] = {0, 1, 1, 1, 2, 6, 7, 8, 2, 3, 4, 5, 2, 9, 10, 11};
		node.bimgoffset += wireOffsets[bits];
	}
	else if ((blockID == 104 || blockID == 105) && blockData == 7)  // full stem
	{
		Block blockW = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(-1,0,0));
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,1,0));
		Block blockN = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		int target = (blockID == 104) ? 86 : 103;
		if (blockN.id == target)
			node.bimgoffset += 1;
		else if (blockS.id == target)
			node.bimgoffset += 2;
		else if (blockW.id == target)
			node.bimgoffset += 3;
		else if (blockE.id == target)
			node.bimgoffset += 4;
	}
	else if (blockID == 64 || blockID == 71 || blockID == 193 || blockID == 194 || blockID == 195 || blockID == 196 || blockID == 197)  // wooden door, iron door, other wooden doors
	{
		Block blockU = getNeighborUD(chunkdata, bi + BlockIdx(0,0,1));
		Block blockD = getNeighborUD(chunkdata, bi + BlockIdx(0,0,-1));
		bool isTop = blockD.id == blockID;
		uint8_t blockDataTop = isTop ? blockData : blockU.data;
		uint8_t blockDataBottom = isTop ? blockD.data : blockData;
		int dir = blockDataBottom % 4;
		if (blockDataBottom & 0x4)
			dir = (dir + ((blockDataTop & 0x1) ? 3 : 1)) % 4;
		node.bimgoffset += isTop ? (dir + 4) : dir;
	}

	//!!!!!!!! for now, only fully opaque blocks can have drop-off shadows, but some others like snow could
	//          probably use them, too
	if (rj.blockimages.isOpaque(node.bimgoffset))
	{
		Block blockS = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(1,0,0));
		Block blockE = getNeighbor(chunkdata, rj, ci, bi + BlockIdx(0,-1,0));
		Block blockD = getNeighborUD(chunkdata, bi + BlockIdx(0,0,-1));

		//!!!!!! neighboring blocks that aren't full height like snow and half-steps should probably produce
		//        the drop-off effect, too
		//!!!!!!! not to mention fully-transparent block images
		if (blockS.id == 0)  // air
			node.darkenSU = true;
		if (blockE.id == 0)  // air
			node.darkenEU = true;
		if (blockD.id == 0)  // air
		{
			node.darkenND = true;
			node.darkenWD = true;
		}
	}
}

//!!!!!! speed these up--lots of conditionals at the moment
void darkenEUEdge(RGBAImage& img, int32_t xstart, int32_t ystart, int B)
{
	// EU edge starts at [2B-1,0] and goes one step DL, then one step L, etc., for a total of 2B-1 steps
	int32_t x = xstart + 2*B-1, y = ystart;
	bool which = true;
	for (int i = 0; i < 2*B-1; i++)
	{
		if (x >= 0 && x < img.w && y >= 0 && y < img.h)
			blend(img(x, y), 0x60000000);
		x--;
		if (which)
			y++;
		which = !which;
	}
}
void darkenSUEdge(RGBAImage& img, int32_t xstart, int32_t ystart, int B)
{
	// SU edge starts at [2B,0] and goes one step DR, then one step R, etc., for a total of 2B-1 steps
	int32_t x = xstart + 2*B, y = ystart;
	bool which = true;
	for (int i = 0; i < 2*B-1; i++)
	{
		if (x >= 0 && x < img.w && y >= 0 && y < img.h)
			blend(img(x, y), 0x60000000);
		x++;
		if (which)
			y++;
		which = !which;
	}
}
void darkenNDEdge(RGBAImage& img, int32_t xstart, int32_t ystart, int B)
{
	// ND edge starts at [2B-1,4B-1] and goes one step UL, then one step L, etc., for a total of 2B-1 steps
	int32_t x = xstart + 2*B-1, y = ystart + 4*B-1;
	bool which = true;
	for (int i = 0; i < 2*B-1; i++)
	{
		if (x >= 0 && x < img.w && y >= 0 && y < img.h)
			blend(img(x, y), 0x60000000);
		x--;
		if (which)
			y--;
		which = !which;
	}
}
void darkenWDEdge(RGBAImage& img, int32_t xstart, int32_t ystart, int B)
{
	// WD edge starts at [2B,4B-1] and goes one step UR, then one step R, etc., for a total of 2B-1 steps
	int32_t x = xstart + 2*B, y = ystart + 4*B-1;
	bool which = true;
	for (int i = 0; i < 2*B-1; i++)
	{
		if (x >= 0 && x < img.w && y >= 0 && y < img.h)
			blend(img(x, y), 0x60000000);
		x++;
		if (which)
			y--;
		which = !which;
	}
}

void drawNode(SceneGraphNode& node, RGBAImage& img, const BlockImages& blockimages)
{
	alphablit(blockimages.img, blockimages.getRect(node.bimgoffset), img, node.xstart, node.ystart);
	if (node.darkenEU)
		darkenEUEdge(img, node.xstart, node.ystart, blockimages.rectsize / 4);
	if (node.darkenSU)
		darkenSUEdge(img, node.xstart, node.ystart, blockimages.rectsize / 4);
	if (node.darkenND)
		darkenNDEdge(img, node.xstart, node.ystart, blockimages.rectsize / 4);
	if (node.darkenWD)
		darkenWDEdge(img, node.xstart, node.ystart, blockimages.rectsize / 4);
	node.drawn = true;
}

void drawSubgraph(SceneGraph& sg, int rootnode, RGBAImage& img, const BlockImages& blockimages)
{
	if (sg.nodes[rootnode].drawn)
		return;
	vector<int>& stack = sg.nodestack;
	stack.clear();
	stack.push_back(rootnode);
	while (!stack.empty())
	{
		SceneGraphNode& node = sg.nodes[stack.back()];
		bool pushed = false;
		for (int i = 0; i < 7; i++)
			if (node.children[i] != -1 && !sg.nodes[node.children[i]].drawn)
			{
				stack.push_back(node.children[i]);
				pushed = true;
				break;
			}
		if (pushed)
			continue;
		drawNode(node, img, blockimages);
		stack.pop_back();
	}
}

//!!!!!!!!!!!!! many opportunities for optimization in here
bool renderTile(const TileIdx& ti, RenderJob& rj, RGBAImage& tile)
{
	// if this tile isn't required, abort
	if (!rj.tiletable->isRequired(ti))
		return false;

	// if this tile doesn't fit in the Google map, skip it
	string tilefile = rj.outputpath + "/" + ti.toFilePath(rj.mp);
	if (tilefile.empty())
	{
		cerr << "tile [" << ti.x << "," << ti.y << "] exceeds the possible map size!  skipping..." << endl;
		return false;
	}
	// if we've somehow already drawn this tile (which should not be possible!), skip it
	if (rj.tiletable->isDrawn(ti))
	{
		cerr << "attempted to draw tile [" << ti.x << "," << ti.y << "] more than once!" << endl;
		return false;
	}
	
	// mark this tile drawn
	rj.tiletable->setDrawn(ti);

	// if we're in test mode, don't actually draw anything
	if (rj.testmode)
		return true;

	SceneGraph& sg = *rj.scenegraph;
	sg.clear();
	tile.create(rj.mp.tileSize(), rj.mp.tileSize());
	const BlockImages& blockimages = rj.blockimages;

	// we'll be given block center pixels in absolute coords, but for blitting, we need the block bounding box
	//  in tile image coords; compute the translation that gives us that
	// (subtract the tile bounding box corner, then subtract another [2B,2B] to convert from block center to box)
	BBox tilebb = ti.getBBox(rj.mp);
	int64_t xoff = -tilebb.topLeft.x - 2*rj.mp.B;
	int64_t yoff = -tilebb.topLeft.y - 2*rj.mp.B;

	// step 1: build the scene graph
	// ...we'll iterate through the pseudocolumn center pixels, starting in the top left of the image, moving down then
	//  right; this means that by the time we reach a pseudocolumn, its N, E, and SE neighbors have already been done,
	//  so we can add any necessary edges to or from those neighbors
	for (TileBlockIterator tbit(ti, rj.mp); !tbit.end; tbit.advance())
	{
		// we'll start at the top of the pseudocolumn and go down, adding any non-air blocks to the graph, stopping
		//  at the first totally opaque block
		sg.pcols.push_back(-1);
		PosChunkIdx lastci(-1,-1);
		ChunkData *chunkdata = NULL;
		int prevnode = -1;
		for (PseudocolumnIterator pcit(tbit.current, rj.mp); !pcit.end; pcit.advance())
		{
			// look up chunk data (we might have it already)
			PosChunkIdx ci = pcit.current.getChunkIdx();
			if (ci != lastci)
				chunkdata = rj.chunkcache->getData(ci);

			// get block type
			uint16_t blockID = chunkdata->id(pcit.current);

			// if this is air, move on (we *always* consider air to be transparent; it has no block image)
			if (blockID == 0)
				continue;
				
			// get the block data
			uint8_t blockData = chunkdata->data(pcit.current);
			int initialoffset = blockimages.getOffset(blockID, blockData);  // we might use a different one after checkSpecial

			// create a node for this block
			SceneGraphNode node(tbit.current.x + xoff, tbit.current.y + yoff, pcit.current, initialoffset);

			// check out neighboring blocks to see if we need to do anything special: set the darken-edge flags,
			//  or change the offset to a special one (one not corresponding to a plain blockID/blockData combo)
			checkSpecial(node, blockID, blockData, ci, chunkdata, rj);

			// if this is not air, but is nonetheless transparent, move on
			if (blockimages.isTransparent(node.bimgoffset))
				continue;

			// commit the node
			int thisnode = sg.nodes.size();
			sg.nodes.push_back(node);

			// link our parent (the node above us in our own pseudocolumn) to us
			if (prevnode != -1)
				sg.nodes[prevnode].children[0] = thisnode;
			// ...if we have no parent, then we're the top of this pcol
			else
				sg.pcols.back() = thisnode;
			prevnode = thisnode;

			// if this block is opaque, we're done with this pcol
			if (blockimages.isOpaque(node.bimgoffset))
				break;
		}

		// check dependencies with our N, E, and SE neighbors
		if (tbit.nextN != -1)
			buildDependencies(sg, tbit.nextN, tbit.pos, 4);
		if (tbit.nextE != -1)
			buildDependencies(sg, tbit.nextE, tbit.pos, 5);
		if (tbit.nextSE != -1)
			buildDependencies(sg, tbit.nextSE, tbit.pos, 6);
	}
	
	// if we didn't find anything to draw--i.e. our final image will be fully transparent--then there's
	//  no sense saving it to disk
	if (sg.nodes.empty())
		return false;

	// step 2: traverse the graph and draw the image
	for (int i = 0; i < (int)sg.nodes.size(); i++)
		drawSubgraph(sg, i, tile, blockimages);

	// save the image to disk
	if (!tile.writeImage(tilefile))
		cerr << "failed to write " << tilefile << endl;
	return true;
}



bool renderZoomTile(const ZoomTileIdx& zti, RenderJob& rj, RGBAImage& tile)
{
	// if this is a base tile, render it
	if (zti.zoom == rj.mp.baseZoom)
		return renderTile(zti.toTileIdx(rj.mp), rj, tile);

	// see whether this entire tile can be rejected early
	if (rj.tiletable->reject(zti, rj.mp))
		return false;

	// render the four subtiles (if they're needed)
	TileCache::ZoomLevel& zlevel = rj.tilecache->levels[rj.mp.baseZoom - zti.zoom - 1];
	ZoomTileIdx topleft = zti.toZoom(zti.zoom + 1);
	zlevel.used[0] = renderZoomTile(topleft, rj, zlevel.tiles[0]);
	zlevel.used[1] = renderZoomTile(topleft.add(0,1), rj, zlevel.tiles[1]);
	zlevel.used[2] = renderZoomTile(topleft.add(1,0), rj, zlevel.tiles[2]);
	zlevel.used[3] = renderZoomTile(topleft.add(1,1), rj, zlevel.tiles[3]);

	// if none of the subtiles are used, we have nothing to do
	int usedcount = 0;
	for (int i = 0; i < 4; i++)
		if (zlevel.used[i])
			usedcount++;
	if (usedcount == 0)
		return false;

	// if we're in test mode, pretend we've successfully drawn
	if (rj.testmode)
		return true;

	// if some of the subtiles are unused and this is an incremental update, we need to
	//  load the existing version of this tile (if there is one) to get the unchanged portions
	string tilefile = rj.outputpath + "/" + zti.toFilePath();
	if (usedcount < 4 && !rj.fullrender)
	{
		// if it doesn't read, no big deal (it may not exist anyway)
		if (!tile.readPNG(tilefile + ".png") || tile.w != rj.mp.tileSize() || tile.h != rj.mp.tileSize())
			tile.create(rj.mp.tileSize(), rj.mp.tileSize());
	}
	else
		tile.create(rj.mp.tileSize(), rj.mp.tileSize());

	// combine the four subtile images into this tile's image
	int halfsize = rj.mp.tileSize() / 2;
	if (zlevel.used[0])
		reduceHalf(tile, ImageRect(0, 0, halfsize, halfsize), zlevel.tiles[0]);
	if (zlevel.used[1])
		reduceHalf(tile, ImageRect(0, halfsize, halfsize, halfsize), zlevel.tiles[1]);
	if (zlevel.used[2])
		reduceHalf(tile, ImageRect(halfsize, 0, halfsize, halfsize), zlevel.tiles[2]);
	if (zlevel.used[3])
		reduceHalf(tile, ImageRect(halfsize, halfsize, halfsize, halfsize), zlevel.tiles[3]);

	// save to disk
	if (!tile.writeImage(tilefile))
		cerr << "failed to write " << tilefile << endl;
	return true;
}



bool renderZoomTile(const ZoomTileIdx& zti, RenderJob& rj, RGBAImage& tile, const ThreadOutputCache& tocache)
{
	// if this is at or below the ThreadOutputCache level, abort
	if (zti.zoom >= tocache.zoom)
		return false;

	// get the four subtiles: if we're one level above the ThreadOutputCache level, they're in the
	//  cache; otherwise, we recurse
	// ...we use the ZoomLevel::used bits either way
	TileCache::ZoomLevel& zlevel = rj.tilecache->levels[rj.mp.baseZoom - zti.zoom - 1];
	ZoomTileIdx topleft = zti.toZoom(zti.zoom + 1);
	const RGBAImage *tile0, *tile1, *tile2, *tile3;
	if (zti.zoom == tocache.zoom - 1)
	{
		int idx = tocache.getIndex(topleft);
		zlevel.used[0] = tocache.used[idx];
		tile0 = &tocache.images[idx];
		idx = tocache.getIndex(topleft.add(0,1));
		zlevel.used[1] = tocache.used[idx];
		tile1 = &tocache.images[idx];
		idx = tocache.getIndex(topleft.add(1,0));
		zlevel.used[2] = tocache.used[idx];
		tile2 = &tocache.images[idx];
		idx = tocache.getIndex(topleft.add(1,1));
		zlevel.used[3] = tocache.used[idx];
		tile3 = &tocache.images[idx];
	}
	else
	{
		zlevel.used[0] = renderZoomTile(topleft, rj, zlevel.tiles[0], tocache);
		tile0 = &zlevel.tiles[0];
		zlevel.used[1] = renderZoomTile(topleft.add(0,1), rj, zlevel.tiles[1], tocache);
		tile1 = &zlevel.tiles[1];
		zlevel.used[2] = renderZoomTile(topleft.add(1,0), rj, zlevel.tiles[2], tocache);
		tile2 = &zlevel.tiles[2];
		zlevel.used[3] = renderZoomTile(topleft.add(1,1), rj, zlevel.tiles[3], tocache);
		tile3 = &zlevel.tiles[3];
	}

	// if none of the subtiles are used, we have nothing to do
	int usedcount = 0;
	for (int i = 0; i < 4; i++)
		if (zlevel.used[i])
			usedcount++;
	if (usedcount == 0)
		return false;

	// if we're in test mode, pretend we've successfully drawn
	if (rj.testmode)
		return true;

	// if some of the subtiles are unused and this is an incremental update, we need to
	//  load the existing version of this tile (if there is one) to get the unchanged portions
	string tilefile = rj.outputpath + "/" + zti.toFilePath();
	if (usedcount < 4 && !rj.fullrender)
	{
		// if it doesn't read, no big deal (it may not exist anyway)
		if (!tile.readPNG(tilefile + ".png") || tile.w != rj.mp.tileSize() || tile.h != rj.mp.tileSize())
			tile.create(rj.mp.tileSize(), rj.mp.tileSize());
	}
	else
		tile.create(rj.mp.tileSize(), rj.mp.tileSize());

	// combine the four subtile images into this tile's image
	int halfsize = rj.mp.tileSize() / 2;
	if (zlevel.used[0])
		reduceHalf(tile, ImageRect(0, 0, halfsize, halfsize), *tile0);
	if (zlevel.used[1])
		reduceHalf(tile, ImageRect(0, halfsize, halfsize, halfsize), *tile1);
	if (zlevel.used[2])
		reduceHalf(tile, ImageRect(halfsize, 0, halfsize, halfsize), *tile2);
	if (zlevel.used[3])
		reduceHalf(tile, ImageRect(halfsize, halfsize, halfsize, halfsize), *tile3);

	// save to disk
	if (!tile.writeImage(tilefile))
		cerr << "failed to write " << tilefile << endl;
	return true;
}





void testTileIterator()
{
	MapParams mp(0,0,0);
	for (mp.B = 2; mp.B <= 6; mp.B++)
		for (mp.T = 1; mp.T <= 4; mp.T++)
		{
			cout << "B = " << mp.B << "   T = " << mp.T << endl;
			for (int64_t tx = -5; tx <= 5; tx++)
				for (int64_t ty = -5; ty <= 5; ty++)
				{
					// get computed BBox
					TileIdx ti(tx,ty);
					BBox bbox = ti.getBBox(mp);

					// use TileBlockIterator to go through the block centers in the tile; verify that
					//  each is actually in the tile by getting the topmost block with that center and checking
					//  its bounding box against the tile's box
					// ...and also make sure that the N, E, SE neighbors are where they're supposed to be
					vector<BlockIdx> blocks;
					for (TileBlockIterator it(ti, mp); !it.end; it.advance())
					{
						BlockIdx bi = BlockIdx::topBlock(it.current, mp);
						//cout << "[" << bi.x << "," << bi.z << "," << bi.y << "]   pos " << it.pos << "   nextE " << it.nextE << "   nextN " << it.nextN << "   nextSE " << it.nextSE << endl;
						if (bi.getCenter(mp) != it.current)
						{
							cout << "topBlock mismatch: [" << it.current.x << "," << it.current.y << "] -> [" << bi.x << "," << bi.z << "," << bi.y << "] -> [" << bi.getCenter(mp).x << "," << bi.getCenter(mp).y << "]" << endl;
							return;
						}
						if (!bi.getBBox(mp).overlaps(bbox))
						{
							cout << "block centered at [" << it.current.x << "," << it.current.y << "] is not in tile!" << endl;
							cout << "[" << bbox.topLeft.x << "," << bbox.topLeft.y << "] to [" << bbox.bottomRight.x << "," << bbox.bottomRight.y << "]" << endl;
							return;
						}
						if ((unsigned) it.pos != blocks.size())
						{
							cout << "block position seems to have advanced too fast!" << endl;
							return;
						}
						blocks.push_back(bi);
						if (it.nextE >= it.pos || it.nextN >= it.pos || it.nextSE >= it.pos)
						{
							cout << "neighbor position is *after* us!" << endl;
							return;
						}
						if (it.nextE != -1 && blocks[it.nextE].z != bi.z - 1)
						{
							cout << "E neighbor pos is wrong" << endl;
							return;
						}
						if (it.nextN != -1 && blocks[it.nextN].x != bi.x - 1)
						{
							cout << "N neighbor pos is wrong" << endl;
							return;
						}
						if (it.nextSE != -1 && (blocks[it.nextSE].z != bi.z - 1 || blocks[it.nextSE].x != bi.x + 1))
						{
							cout << "SE neighbor pos is wrong" << endl;
							return;
						}
					}
				}
		}
}

void testPColIterator()
{
	MapParams mp(6,1,0);
	for (int64_t tx = -5; tx <= 5; tx++)
		for (int64_t ty = -5; ty <= 5; ty++)
		{
			TileIdx ti(tx,ty);
			vector<Pixel> centers;
			for (TileBlockIterator tbit(ti, mp); !tbit.end; tbit.advance())
			{
				centers.push_back(tbit.current);

				// check this pseudocolumn against its N, E, SE neighbors; make sure the blocks
				//  chosen by the iterator actually have the proper relationships
				auto_ptr<PseudocolumnIterator> nit((tbit.nextN != -1) ? new PseudocolumnIterator(centers[tbit.nextN], mp) : NULL);
				auto_ptr<PseudocolumnIterator> eit((tbit.nextE != -1) ? new PseudocolumnIterator(centers[tbit.nextE], mp) : NULL);
				auto_ptr<PseudocolumnIterator> seit((tbit.nextSE != -1) ? new PseudocolumnIterator(centers[tbit.nextSE], mp) : NULL);
				for (PseudocolumnIterator pcit(tbit.current, mp); !pcit.end; pcit.advance())
				{
					if (nit.get() != NULL)
					{
						if (nit->current != pcit.current + BlockIdx(-1,0,0))
							cout << "N pcol iterator block is not actually N neighbor!" << endl;
						if (nit->current.getCenter(mp) != pcit.current.getCenter(mp) + Pixel(-2*mp.B, mp.B))
							cout << "N neighbor pixel is wrong!" << endl;
						nit->advance();
					}
					if (eit.get() != NULL)
					{
						if (eit->current != pcit.current + BlockIdx(0,-1,0))
							cout << "E pcol iterator block is not actually E neighbor!" << endl;
						if (eit->current.getCenter(mp) != pcit.current.getCenter(mp) + Pixel(-2*mp.B, -mp.B))
							cout << "E neighbor pixel is wrong!" << endl;
						eit->advance();
					}
					if (seit.get() != NULL)
					{
						if (seit->current != pcit.current + BlockIdx(1,-1,0))
							cout << "SE pcol iterator block is not actually SE neighbor!" << endl;
						if (seit->current.getCenter(mp) != pcit.current.getCenter(mp) + Pixel(0, -2*mp.B))
							cout << "SE neighbor pixel is wrong!" << endl;
						seit->advance();
					}
				}
			}
		}
}
