#include "animation/Svg.h"
#include "animation/Animation.h"
#include "utils/CMath.h"
#include "renderer/Renderer.h"
#include "renderer/Framebuffer.h"
#include "renderer/Texture.h"
#include "renderer/Colors.h"
#include "renderer/PerspectiveCamera.h"
#include "core/Application.h"

#include <nanovg.h>

namespace MathAnim
{
	static float cacheLineHeight;
	static Vec2 cachePadding = { 3, 3 };
	static Vec2 cacheCurrentPos;
	static Framebuffer svgCache;

	namespace Svg
	{
		// ----------------- Private Variables -----------------
		constexpr int initialMaxCapacity = 5;
		static OrthoCamera* orthoCamera;
		static PerspectiveCamera* perspCamera;
		static Vec2 cursor;
		static bool moveToP0 = false;

		// ----------------- Internal functions -----------------
		static void checkResize(Contour& contour);
		static void render2DInterpolation(NVGcontext* vg, const AnimObject* animObjectSrc, const SvgObject* interpolationSrc, const AnimObject* animObjectDst, const SvgObject* interpolationDst, float t);
		static void generateSvgCache(uint32 width, uint32 height);

		SvgObject createDefault()
		{
			SvgObject res;
			res.approximatePerimeter = 0.0f;
			// Dummy allocation to allow memory tracking when reallocing 
			// TODO: Fix the dang memory allocation library so I don't have to do this!
			res.contours = (Contour*)g_memory_allocate(sizeof(Contour));
			res.numContours = 0;
			return res;
		}

		SvgGroup createDefaultGroup()
		{
			SvgGroup res;
			// Dummy allocation to allow memory tracking when reallocing 
			// TODO: Fix the dang memory allocation library so I don't have to do this!
			res.objects = (SvgObject*)g_memory_allocate(sizeof(SvgObject));
			res.objectOffsets = (Vec2*)g_memory_allocate(sizeof(Vec2));
			res.numObjects = 0;
			res.uniqueObjects = (SvgObject*)g_memory_allocate(sizeof(SvgObject));
			res.uniqueObjectNames = (char**)g_memory_allocate(sizeof(char*));
			res.numUniqueObjects = 0;
			res.viewbox = Vec4{ 0, 0, 1, 1 };
			return res;
		}

		void init(OrthoCamera& sceneCamera2d, PerspectiveCamera& sceneCamera3d)
		{
			orthoCamera = &sceneCamera2d;
			perspCamera = &sceneCamera3d;

			constexpr int defaultWidth = 4096;
			generateSvgCache(defaultWidth, defaultWidth);

			cacheCurrentPos.x = 0;
			cacheCurrentPos.y = 0;
		}

		void free()
		{
			if (svgCache.fbo != UINT32_MAX)
			{
				svgCache.destroy();
			}
		}

		void endFrame()
		{
			cacheCurrentPos.x = 0;
			cacheCurrentPos.y = 0;

			svgCache.bind();
			glViewport(0, 0, svgCache.width, svgCache.height);
			//svgCache.clearColorAttachmentRgba(0, "#fc03ecFF"_hex);
			svgCache.clearColorAttachmentRgba(0, "#00000000"_hex);
			svgCache.clearDepthStencil();
		}

		const Texture& getSvgCache()
		{
			return svgCache.getColorAttachment(0);
		}

		Framebuffer const& getSvgCacheFb()
		{
			return svgCache;
		}

		const Vec2& getCacheCurrentPos()
		{
			return cacheCurrentPos;
		}

		const Vec2& getCachePadding()
		{
			return cachePadding;
		}

		void incrementCacheCurrentY()
		{
			cacheCurrentPos.y += cacheLineHeight + cachePadding.y;
			cacheLineHeight = 0;
			cacheCurrentPos.x = 0;
		}

		void incrementCacheCurrentX(float distance)
		{
			cacheCurrentPos.x += distance;
		}

		void checkLineHeight(float newLineHeight)
		{
			cacheLineHeight = glm::max(cacheLineHeight, newLineHeight);
		}

		void growCache()
		{
			// Double the size of the texture (up to 8192x8192 max)
			Svg::generateSvgCache(svgCache.width * 2, svgCache.height * 2);
		}

		PerspectiveCamera const& getPerspCamera()
		{
			return *perspCamera;
		}

		OrthoCamera const& getOrthoCamera()
		{
			return *orthoCamera;
		}

		void beginSvgGroup(SvgGroup* group, const Vec4& viewbox)
		{
			group->viewbox = viewbox;
		}

		void pushSvgToGroup(SvgGroup* group, const SvgObject& obj, const std::string& id, const Vec2& offset)
		{
			group->numObjects++;
			group->objects = (SvgObject*)g_memory_realloc(group->objects, sizeof(SvgObject) * group->numObjects);
			g_logger_assert(group->objects != nullptr, "Ran out of RAM.");
			group->objectOffsets = (Vec2*)g_memory_realloc(group->objectOffsets, sizeof(Vec2) * group->numObjects);
			g_logger_assert(group->objectOffsets != nullptr, "Ran out of RAM.");

			group->objectOffsets[group->numObjects - 1] = offset;
			group->objects[group->numObjects - 1] = obj;

			// Horribly inefficient... do something better eventually
			bool isUnique = true;
			for (int i = 0; i < group->numUniqueObjects; i++)
			{
				if (std::strcmp(group->uniqueObjectNames[i], id.c_str()) == 0)
				{
					isUnique = false;
				}
			}
			if (isUnique)
			{
				group->numUniqueObjects++;
				group->uniqueObjectNames = (char**)g_memory_realloc(group->uniqueObjectNames, sizeof(char**) * group->numUniqueObjects);
				g_logger_assert(group->uniqueObjectNames != nullptr, "Ran out of RAM.");
				group->uniqueObjects = (SvgObject*)g_memory_realloc(group->uniqueObjects, sizeof(SvgObject) * group->numUniqueObjects);
				g_logger_assert(group->uniqueObjects != nullptr, "Ran out of RAM.");

				group->uniqueObjects[group->numUniqueObjects - 1] = obj;
				group->uniqueObjectNames[group->numUniqueObjects - 1] = (char*)g_memory_allocate(sizeof(char) * (id.length() + 1));
				g_memory_copyMem(group->uniqueObjectNames[group->numUniqueObjects - 1], (void*)id.c_str(), id.length() * sizeof(char));
				group->uniqueObjectNames[group->numUniqueObjects - 1][id.length()] = '\0';
			}
		}

		void endSvgGroup(SvgGroup* group)
		{
			Vec3 viewboxPos = Vec3{ group->viewbox.values[0], group->viewbox.values[1], 0.0f };
			Vec3 viewboxSize = Vec3{ group->viewbox.values[2], group->viewbox.values[3], 1.0f };

			// Normalize all the SVGs within the viewbox
			//for (int svgi = 0; svgi < group->numObjects; svgi++)
			//{
			//	// First get the original SVG element size recorded
			//	group->objects[svgi].calculateSvgSize();
			//	// Then normalize it and make sure the perimeter is calculated
			//	//group->objects[svgi].normalize();
			//	group->objects[svgi].calculateApproximatePerimeter();
			//	group->objects[svgi].calculateBBox();
			//}
			group->normalize();
		}

		void beginContour(SvgObject* object, const Vec2& firstPoint)
		{
			object->numContours++;
			object->contours = (Contour*)g_memory_realloc(object->contours, sizeof(Contour) * object->numContours);
			g_logger_assert(object->contours != nullptr, "Ran out of RAM.");

			object->contours[object->numContours - 1].maxCapacity = initialMaxCapacity;
			object->contours[object->numContours - 1].curves = (Curve*)g_memory_allocate(sizeof(Curve) * initialMaxCapacity);
			object->contours[object->numContours - 1].numCurves = 0;
			object->contours[object->numContours - 1].isHole = false;

			object->contours[object->numContours - 1].curves[0].p0 = firstPoint;
			cursor = firstPoint;
			moveToP0 = false;
		}

		void closeContour(SvgObject* object, bool lineToEndpoint, bool isHole)
		{
			g_logger_assert(object->numContours > 0, "object->numContours == 0. Cannot close contour when no contour exists.");
			g_logger_assert(object->contours[object->numContours - 1].numCurves > 0, "contour->numCurves == 0. Cannot close contour with 0 vertices. There must be at least one vertex to close a contour.");

			object->contours[object->numContours - 1].isHole = isHole;
			if (lineToEndpoint)
			{
				if (object->contours[object->numContours - 1].numCurves > 0)
				{
					Vec2 firstPoint = object->contours[object->numContours - 1].curves[0].p0;
					lineTo(object, firstPoint, true);
				}
			}

			cursor = Vec2{ 0, 0 };
		}

		void moveTo(SvgObject* object, const Vec2& point, bool absolute)
		{
			// If no object has started, begin the object here
			if (object->numContours == 0)
			{
				beginContour(object, point);
				absolute = true;
				moveToP0 = false;
			}
			else
			{
				cursor = absolute ? point : cursor + point;
				moveToP0 = true;
			}
		}

		void lineTo(SvgObject* object, const Vec2& point, bool absolute)
		{
			g_logger_assert(object->numContours > 0, "object->numContours == 0. Cannot create a lineTo when no contour exists.");
			Contour& contour = object->contours[object->numContours - 1];
			contour.numCurves++;
			checkResize(contour);

			contour.curves[contour.numCurves - 1].p0 = cursor;
			contour.curves[contour.numCurves - 1].as.line.p1 = absolute ? point : point + cursor;
			contour.curves[contour.numCurves - 1].type = CurveType::Line;
			contour.curves[contour.numCurves - 1].moveToP0 = moveToP0;

			cursor = contour.curves[contour.numCurves - 1].as.line.p1;
			moveToP0 = false;
		}

		void hzLineTo(SvgObject* object, float xPoint, bool absolute)
		{
			Vec2 position = absolute
				? Vec2{ xPoint, cursor.y }
			: Vec2{ xPoint, 0.0f } + cursor;
			lineTo(object, position, true);
		}

		void vtLineTo(SvgObject* object, float yPoint, bool absolute)
		{
			Vec2 position = absolute
				? Vec2{ cursor.x, yPoint }
			: Vec2{ 0.0f, yPoint } + cursor;
			lineTo(object, position, true);
		}

		void bezier2To(SvgObject* object, const Vec2& control, const Vec2& dest, bool absolute)
		{
			g_logger_assert(object->numContours > 0, "object->numContours == 0. Cannot create a bezier2To when no contour exists.");
			Contour& contour = object->contours[object->numContours - 1];
			contour.numCurves++;
			checkResize(contour);

			contour.curves[contour.numCurves - 1].p0 = cursor;

			contour.curves[contour.numCurves - 1].as.bezier2.p1 = absolute ? control : control + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier2.p1;

			contour.curves[contour.numCurves - 1].as.bezier2.p2 = absolute ? dest : dest + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier2.p2;

			contour.curves[contour.numCurves - 1].type = CurveType::Bezier2;
			contour.curves[contour.numCurves - 1].moveToP0 = moveToP0;

			moveToP0 = false;
		}

		void bezier3To(SvgObject* object, const Vec2& control0, const Vec2& control1, const Vec2& dest, bool absolute)
		{
			g_logger_assert(object->numContours > 0, "object->numContours == 0. Cannot create a bezier3To when no contour exists.");
			Contour& contour = object->contours[object->numContours - 1];
			contour.numCurves++;
			checkResize(contour);

			contour.curves[contour.numCurves - 1].p0 = cursor;

			contour.curves[contour.numCurves - 1].as.bezier3.p1 = absolute ? control0 : control0 + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier3.p1;

			contour.curves[contour.numCurves - 1].as.bezier3.p2 = absolute ? control1 : control1 + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier3.p2;

			contour.curves[contour.numCurves - 1].as.bezier3.p3 = absolute ? dest : dest + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier3.p3;

			contour.curves[contour.numCurves - 1].type = CurveType::Bezier3;
			contour.curves[contour.numCurves - 1].moveToP0 = moveToP0;

			moveToP0 = false;
		}

		void smoothBezier2To(SvgObject* object, const Vec2& dest, bool absolute)
		{
			g_logger_assert(object->numContours > 0, "object->numContours == 0. Cannot create a bezier3To when no contour exists.");
			Contour& contour = object->contours[object->numContours - 1];
			contour.numCurves++;
			checkResize(contour);

			contour.curves[contour.numCurves - 1].p0 = cursor;

			Vec2 control0 = cursor;
			if (contour.numCurves > 1)
			{
				if (contour.curves[contour.numCurves - 2].type == CurveType::Bezier2)
				{
					Vec2 prevControl1 = contour.curves[contour.numCurves - 2].as.bezier2.p1;
					// Reflect the previous c2 about the current cursor
					control0 = (-1.0f * (prevControl1 - cursor)) + cursor;
				}
			}
			contour.curves[contour.numCurves - 1].as.bezier2.p1 = control0;
			cursor = contour.curves[contour.numCurves - 1].as.bezier2.p1;

			contour.curves[contour.numCurves - 1].as.bezier2.p2 = absolute ? dest : dest + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier2.p2;

			contour.curves[contour.numCurves - 1].type = CurveType::Bezier2;
			contour.curves[contour.numCurves - 1].moveToP0 = moveToP0;

			moveToP0 = false;
		}

		void smoothBezier3To(SvgObject* object, const Vec2& control1, const Vec2& dest, bool absolute)
		{
			g_logger_assert(object->numContours > 0, "object->numContours == 0. Cannot create a bezier3To when no contour exists.");
			Contour& contour = object->contours[object->numContours - 1];
			contour.numCurves++;
			checkResize(contour);

			contour.curves[contour.numCurves - 1].p0 = cursor;

			Vec2 control0 = cursor;
			if (contour.numCurves > 1)
			{
				if (contour.curves[contour.numCurves - 2].type == CurveType::Bezier3)
				{
					Vec2 prevControl1 = contour.curves[contour.numCurves - 2].as.bezier3.p2;
					// Reflect the previous c2 about the current cursor
					control0 = (-1.0f * (prevControl1 - cursor)) + cursor;
				}
			}
			contour.curves[contour.numCurves - 1].as.bezier3.p1 = control0;
			cursor = contour.curves[contour.numCurves - 1].as.bezier3.p1;

			contour.curves[contour.numCurves - 1].as.bezier3.p2 = absolute ? control1 : control1 + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier3.p2;

			contour.curves[contour.numCurves - 1].as.bezier3.p3 = absolute ? dest : dest + cursor;
			cursor = contour.curves[contour.numCurves - 1].as.bezier3.p3;

			contour.curves[contour.numCurves - 1].type = CurveType::Bezier3;
			contour.curves[contour.numCurves - 1].moveToP0 = moveToP0;

			moveToP0 = false;
		}

		void arcTo(SvgObject* object, const Vec2& radius, float xAxisRot, bool largeArc, bool sweep, const Vec2& dst, bool absolute)
		{
			// TODO: All this should probably be implementation details...
			if (CMath::compare(cursor, dst))
			{
				// If the cursor == dst then no arc is emitted
				return;
			}

			if (CMath::compare(radius.x, 0.0f) || CMath::compare(radius.y, 0.0f))
			{
				// Treat the arc as a line to if the radius x or y is 0
				lineTo(object, dst, absolute);
				return;
			}

			// TMP: Implement actual arcTo functionality
			lineTo(object, dst, absolute);

			// float rx = glm::abs(radius.x);
			// float ry = glm::abs(radius.y);
			// xAxisRot = fmodf(xAxisRot, 360.0f);
		}

		void copy(SvgObject* dest, const SvgObject* src)
		{
			if (dest->numContours != src->numContours)
			{
				// Free any extra contours the destination has
				// If the destination has less, this loop doesn't run
				for (int contouri = src->numContours; contouri < dest->numContours; contouri++)
				{
					g_memory_free(dest->contours[contouri].curves);
					dest->contours[contouri].curves = nullptr;
					dest->contours[contouri].numCurves = 0;
					dest->contours[contouri].maxCapacity = 0;
				}

				// Then reallocate memory. If dest had less, this will acquire enough new memory
				// If dest had more, this will get rid of the extra memory
				dest->contours = (Contour*)g_memory_realloc(dest->contours, sizeof(Contour) * src->numContours);

				// Go through and initialize the curves for any new curves that were added
				for (int contouri = dest->numContours; contouri < src->numContours; contouri++)
				{
					dest->contours[contouri].curves = (Curve*)g_memory_allocate(sizeof(Curve) * initialMaxCapacity);
					dest->contours[contouri].maxCapacity = initialMaxCapacity;
					dest->contours[contouri].numCurves = 0;
				}

				// Then set the number of contours equal
				dest->numContours = src->numContours;
			}

			g_logger_assert(dest->numContours == src->numContours, "How did this happen?");

			// Finally we can just go through and set all the data equal to each other
			for (int contouri = 0; contouri < src->numContours; contouri++)
			{
				Contour& dstContour = dest->contours[contouri];
				dstContour.numCurves = 0;

				for (int curvei = 0; curvei < src->contours[contouri].numCurves; curvei++)
				{
					const Curve& srcCurve = src->contours[contouri].curves[curvei];

					dstContour.numCurves++;
					checkResize(dstContour);

					// Copy the data
					Curve& dstCurve = dstContour.curves[curvei];
					dstCurve.p0 = srcCurve.p0;
					dstCurve.as = srcCurve.as;
					dstCurve.type = srcCurve.type;
					dstCurve.moveToP0 = srcCurve.moveToP0;
				}

				g_logger_assert(dstContour.numCurves == src->contours[contouri].numCurves, "How did this happen?");
			}

			dest->calculateApproximatePerimeter();
			dest->calculateBBox();
		}

		void renderInterpolation(NVGcontext* vg, const AnimObject* animObjectSrc, const SvgObject* interpolationSrc, const AnimObject* animObjectDst, const SvgObject* interpolationDst, float t)
		{
			render2DInterpolation(vg, animObjectSrc, interpolationSrc, animObjectDst, interpolationDst, t);
		}

		// ----------------- Internal functions -----------------
		static void checkResize(Contour& contour)
		{
			if (contour.numCurves > contour.maxCapacity)
			{
				contour.maxCapacity *= 2;
				contour.curves = (Curve*)g_memory_realloc(contour.curves, sizeof(Curve) * contour.maxCapacity);
				g_logger_assert(contour.curves != nullptr, "Ran out of RAM.");
			}
		}

		static void render2DInterpolation(NVGcontext* vg, const AnimObject* animObjectSrc, const SvgObject* interpolationSrc, const AnimObject* animObjectDst, const SvgObject* interpolationDst, float t)
		{
			// Interpolate fill color
			glm::vec4 fillColorSrc = glm::vec4(
				(float)animObjectSrc->fillColor.r / 255.0f,
				(float)animObjectSrc->fillColor.g / 255.0f,
				(float)animObjectSrc->fillColor.b / 255.0f,
				(float)animObjectSrc->fillColor.a / 255.0f
			);
			glm::vec4 fillColorDst = glm::vec4(
				(float)animObjectDst->fillColor.r / 255.0f,
				(float)animObjectDst->fillColor.g / 255.0f,
				(float)animObjectDst->fillColor.b / 255.0f,
				(float)animObjectDst->fillColor.a / 255.0f
			);
			glm::vec4 fillColorInterp = (fillColorDst - fillColorSrc) * t + fillColorSrc;
			NVGcolor fillColor = nvgRGBA(
				(uint8)(fillColorInterp.r * 255.0f),
				(uint8)(fillColorInterp.g * 255.0f),
				(uint8)(fillColorInterp.b * 255.0f),
				(uint8)(fillColorInterp.a * 255.0f)
			);

			// Interpolate stroke color
			glm::vec4 strokeColorSrc = glm::vec4(
				(float)animObjectSrc->strokeColor.r / 255.0f,
				(float)animObjectSrc->strokeColor.g / 255.0f,
				(float)animObjectSrc->strokeColor.b / 255.0f,
				(float)animObjectSrc->strokeColor.a / 255.0f
			);
			glm::vec4 strokeColorDst = glm::vec4(
				(float)animObjectDst->strokeColor.r / 255.0f,
				(float)animObjectDst->strokeColor.g / 255.0f,
				(float)animObjectDst->strokeColor.b / 255.0f,
				(float)animObjectDst->strokeColor.a / 255.0f
			);
			glm::vec4 strokeColorInterp = (strokeColorDst - strokeColorSrc) * t + strokeColorSrc;
			NVGcolor strokeColor = nvgRGBA(
				(uint8)(strokeColorInterp.r * 255.0f),
				(uint8)(strokeColorInterp.g * 255.0f),
				(uint8)(strokeColorInterp.b * 255.0f),
				(uint8)(strokeColorInterp.a * 255.0f)
			);

			// Interpolate position
			const Vec2& dstPos = CMath::vector2From3(animObjectDst->position);
			const Vec2& srcPos = CMath::vector2From3(animObjectSrc->position);
			glm::vec2 interpolatedPos = glm::vec2(
				(dstPos.x - srcPos.x) * t + srcPos.x,
				(dstPos.y - srcPos.y) * t + srcPos.y
			);

			// Interpolate rotation
			const Vec3& dstRotation = animObjectDst->rotation;
			const Vec3& srcRotation = animObjectSrc->rotation;
			glm::vec3 interpolatedRotation = glm::vec3(
				(dstRotation.x - srcRotation.x) * t + srcRotation.x,
				(dstRotation.x - srcRotation.y) * t + srcRotation.y,
				(dstRotation.x - srcRotation.z) * t + srcRotation.z
			);

			// Apply transformations
			Vec2 cameraCenteredPos = Svg::orthoCamera->projectionSize / 2.0f - Svg::orthoCamera->position;
			nvgTranslate(vg, interpolatedPos.x - cameraCenteredPos.x, interpolatedPos.y - cameraCenteredPos.y);
			if (interpolatedRotation.z != 0.0f)
			{
				nvgRotate(vg, glm::radians(interpolatedRotation.z));
			}

			// Interpolate stroke width
			float dstStrokeWidth = animObjectDst->strokeWidth;
			if (glm::epsilonEqual(dstStrokeWidth, 0.0f, 0.01f))
			{
				dstStrokeWidth = 5.0f;
			}
			float srcStrokeWidth = animObjectSrc->strokeWidth;
			if (glm::epsilonEqual(srcStrokeWidth, 0.0f, 0.01f))
			{
				srcStrokeWidth = 5.0f;
			}
			float strokeWidth = (dstStrokeWidth - srcStrokeWidth) * t + srcStrokeWidth;

			// If one object has more contours than the other object,
			// then we'll just skip every other contour for the object
			// with less contours and hopefully it looks cool
			const SvgObject* lessContours = interpolationSrc->numContours <= interpolationDst->numContours
				? interpolationSrc
				: interpolationDst;
			const SvgObject* moreContours = interpolationSrc->numContours > interpolationDst->numContours
				? interpolationSrc
				: interpolationDst;
			int numContoursToSkip = glm::max(moreContours->numContours / lessContours->numContours, 1);
			int lessContouri = 0;
			int moreContouri = 0;
			while (lessContouri < lessContours->numContours)
			{
				nvgBeginPath(vg);

				nvgFillColor(vg, fillColor);
				nvgStrokeColor(vg, strokeColor);
				nvgStrokeWidth(vg, strokeWidth);

				const Contour& lessCurves = lessContours->contours[lessContouri];
				const Contour& moreCurves = moreContours->contours[moreContouri];

				// It's undefined to interpolate between two contours if one of the contours has no curves
				bool shouldLoop = moreCurves.numCurves > 0 && lessCurves.numCurves > 0;
				if (shouldLoop)
				{
					// Move to the start, which is the interpolation between both of the
					// first vertices
					const Vec2& p0a = lessCurves.curves[0].p0;
					const Vec2& p0b = moreCurves.curves[0].p0;
					Vec2 interpP0 = {
						(p0b.x - p0a.x) * t + p0a.x,
						(p0b.y - p0a.y) * t + p0a.y
					};

					nvgMoveTo(vg, interpP0.x, interpP0.y);
				}

				int maxNumCurves = glm::max(lessCurves.numCurves, moreCurves.numCurves);
				for (int curvei = 0; curvei < maxNumCurves; curvei++)
				{
					// Interpolate between the two curves, treat both curves
					// as bezier3 curves no matter what to make it easier
					glm::vec2 p1a, p2a, p3a;
					glm::vec2 p1b, p2b, p3b;

					if (curvei > lessCurves.numCurves || curvei > moreCurves.numCurves)
					{
						g_logger_error("Cannot interpolate between two contours with different number of curves yet.");
						break;
					}
					const Curve& lessCurve = lessCurves.curves[curvei];
					const Curve& moreCurve = moreCurves.curves[curvei];

					// First get the control points depending on the type of the curve
					switch (lessCurve.type)
					{
					case CurveType::Bezier3:
						p1a = glm::vec2(lessCurve.as.bezier3.p1.x, lessCurve.as.bezier3.p1.y);
						p2a = glm::vec2(lessCurve.as.bezier3.p2.x, lessCurve.as.bezier3.p2.y);
						p3a = glm::vec2(lessCurve.as.bezier3.p3.x, lessCurve.as.bezier3.p3.y);
						break;
					case CurveType::Bezier2:
					{
						glm::vec2 p0 = glm::vec2(lessCurve.p0.x, lessCurve.p0.y);
						glm::vec2 p1 = glm::vec2(lessCurve.as.bezier2.p1.x, lessCurve.as.bezier2.p1.y);
						glm::vec2 p2 = glm::vec2(lessCurve.as.bezier2.p2.x, lessCurve.as.bezier2.p2.y);

						// Degree elevated quadratic bezier curve
						p1a = (1.0f / 3.0f) * p0 + (2.0f / 3.0f) * p1;
						p2a = (2.0f / 3.0f) * p1 + (1.0f / 3.0f) * p2;
						p3a = p2;
					}
					break;
					case CurveType::Line:
						p1a = glm::vec2(lessCurve.p0.x, lessCurve.p0.y);
						p2a = glm::vec2(lessCurve.as.line.p1.x, lessCurve.as.line.p1.y);
						p3a = p2a;
						break;
					default:
						g_logger_warning("Unknown curve type %d", lessCurve.type);
						break;
					}

					switch (moreCurve.type)
					{
					case CurveType::Bezier3:
						p1b = glm::vec2(moreCurve.as.bezier3.p1.x, moreCurve.as.bezier3.p1.y);
						p2b = glm::vec2(moreCurve.as.bezier3.p2.x, moreCurve.as.bezier3.p2.y);
						p3b = glm::vec2(moreCurve.as.bezier3.p3.x, moreCurve.as.bezier3.p3.y);
						break;
					case CurveType::Bezier2:
					{
						glm::vec2 p0 = glm::vec2(moreCurve.p0.x, moreCurve.p0.y);
						glm::vec2 p1 = glm::vec2(moreCurve.as.bezier2.p1.x, moreCurve.as.bezier2.p1.y);
						glm::vec2 p2 = glm::vec2(moreCurve.as.bezier2.p2.x, moreCurve.as.bezier2.p2.y);

						// Degree elevated quadratic bezier curve
						p1b = (1.0f / 3.0f) * p0 + (2.0f / 3.0f) * p1;
						p2b = (2.0f / 3.0f) * p1 + (1.0f / 3.0f) * p2;
						p3b = p2;
					}
					break;
					case CurveType::Line:
						p1b = glm::vec2(moreCurve.p0.x, moreCurve.p0.y);
						p2b = glm::vec2(moreCurve.as.line.p1.x, moreCurve.as.line.p1.y);
						p3b = p2b;
						break;
					default:
						g_logger_warning("Unknown curve type %d", moreCurve.type);
						break;
					}

					// Then interpolate between the control points
					glm::vec2 interpP1 = (p1b - p1a) * t + p1a;
					glm::vec2 interpP2 = (p2b - p2a) * t + p2a;
					glm::vec2 interpP3 = (p3b - p3a) * t + p3a;

					// Then draw
					nvgBezierTo(vg,
						interpP1.x, interpP1.y,
						interpP2.x, interpP2.y,
						interpP3.x, interpP3.y);
				}

				nvgStroke(vg);
				nvgFill(vg);
				nvgClosePath(vg);

				lessContouri++;
				moreContouri += numContoursToSkip;
				if (moreContouri >= moreContours->numContours)
				{
					moreContouri = moreContours->numContours - 1;
				}
			}

			nvgResetTransform(vg);
		}

		static void generateSvgCache(uint32 width, uint32 height)
		{
			if (svgCache.fbo != UINT32_MAX)
			{
				svgCache.destroy();
			}

			if (width > 4096 || height > 4096)
			{
				g_logger_error("SVG cache cannot be bigger than 4096x4096 pixels. The SVG will be truncated.");
				width = 4096;
				height = 4096;
			}

			// Default the svg framebuffer cache to 1024x1024 and resize if necessary
			Texture cacheTexture = TextureBuilder()
				.setFormat(ByteFormat::RGBA8_UI)
				.setMinFilter(FilterMode::Linear)
				.setMagFilter(FilterMode::Linear)
				.setWidth(width)
				.setHeight(height)
				.build();
			svgCache = FramebufferBuilder(width, height)
				.addColorAttachment(cacheTexture)
				.includeDepthStencil()
				.generate();
		}
	}

	// ----------------- SvgObject functions -----------------
	// SvgObject internal functions
	static void renderCreateAnimation2D(NVGcontext* vg, float t, const AnimObject* parent, const Vec2& textureOffset, const Vec2& svgScale, bool reverse, const SvgObject* obj, bool isSvgGroup);

	void SvgObject::normalize(const Vec2& inMin, const Vec2& inMax)
	{
		// First find the min max of the entire curve
		Vec2 min = inMin;
		Vec2 max = inMax;
		if (min.x == FLT_MAX && min.y == FLT_MAX && max.x == FLT_MIN && max.y == FLT_MIN)
		{
			for (int contouri = 0; contouri < this->numContours; contouri++)
			{
				for (int curvei = 0; curvei < this->contours[contouri].numCurves; curvei++)
				{
					const Curve& curve = contours[contouri].curves[curvei];
					const Vec2& p0 = curve.p0;

					min = CMath::min(p0, min);
					max = CMath::max(p0, max);

					switch (curve.type)
					{
					case CurveType::Bezier3:
					{
						const Vec2& p1 = curve.as.bezier3.p1;
						const Vec2& p2 = curve.as.bezier3.p2;
						const Vec2& p3 = curve.as.bezier3.p3;

						min = CMath::min(p1, min);
						max = CMath::max(p1, max);

						min = CMath::min(p2, min);
						max = CMath::max(p2, max);

						min = CMath::min(p3, min);
						max = CMath::max(p3, max);
					}
					break;
					case CurveType::Bezier2:
					{
						const Vec2& p1 = curve.as.bezier2.p1;
						const Vec2& p2 = curve.as.bezier2.p2;

						min = CMath::min(p1, min);
						max = CMath::max(p1, max);

						min = CMath::min(p2, min);
						max = CMath::max(p2, max);
					}
					break;
					case CurveType::Line:
					{
						const Vec2& p1 = curve.as.line.p1;

						min = CMath::min(p1, min);
						max = CMath::max(p1, max);
					}
					break;
					}
				}
			}
		}

		// Then map everything to a [0.0-1.0] range from there
		Vec2 hzOutputRange = Vec2{ 0.0f, 1.0f };
		Vec2 vtOutputRange = Vec2{ 0.0f, 1.0f };
		for (int contouri = 0; contouri < this->numContours; contouri++)
		{
			for (int curvei = 0; curvei < this->contours[contouri].numCurves; curvei++)
			{
				Curve& curve = contours[contouri].curves[curvei];
				curve.p0.x = CMath::mapRange(Vec2{ min.x, max.x }, hzOutputRange, curve.p0.x);
				curve.p0.y = CMath::mapRange(Vec2{ min.y, max.y }, vtOutputRange, curve.p0.y);

				switch (curve.type)
				{
				case CurveType::Bezier3:
				{
					curve.as.bezier3.p1.x = CMath::mapRange(Vec2{ min.x, max.x }, hzOutputRange, curve.as.bezier3.p1.x);
					curve.as.bezier3.p1.y = CMath::mapRange(Vec2{ min.y, max.y }, vtOutputRange, curve.as.bezier3.p1.y);

					curve.as.bezier3.p2.x = CMath::mapRange(Vec2{ min.x, max.x }, hzOutputRange, curve.as.bezier3.p2.x);
					curve.as.bezier3.p2.y = CMath::mapRange(Vec2{ min.y, max.y }, vtOutputRange, curve.as.bezier3.p2.y);

					curve.as.bezier3.p3.x = CMath::mapRange(Vec2{ min.x, max.x }, hzOutputRange, curve.as.bezier3.p3.x);
					curve.as.bezier3.p3.y = CMath::mapRange(Vec2{ min.y, max.y }, vtOutputRange, curve.as.bezier3.p3.y);

				}
				break;
				case CurveType::Bezier2:
				{
					curve.as.bezier2.p1.x = CMath::mapRange(Vec2{ min.x, max.x }, hzOutputRange, curve.as.bezier2.p1.x);
					curve.as.bezier2.p1.y = CMath::mapRange(Vec2{ min.y, max.y }, vtOutputRange, curve.as.bezier2.p1.y);

					curve.as.bezier2.p2.x = CMath::mapRange(Vec2{ min.x, max.x }, hzOutputRange, curve.as.bezier2.p2.x);
					curve.as.bezier2.p2.y = CMath::mapRange(Vec2{ min.y, max.y }, vtOutputRange, curve.as.bezier2.p2.y);
				}
				break;
				case CurveType::Line:
				{
					curve.as.line.p1.x = CMath::mapRange(Vec2{ min.x, max.x }, hzOutputRange, curve.as.line.p1.x);
					curve.as.line.p1.y = CMath::mapRange(Vec2{ min.y, max.y }, vtOutputRange, curve.as.line.p1.y);
				}
				break;
				}
			}
		}
	}

	void SvgObject::calculateApproximatePerimeter()
	{
		approximatePerimeter = 0.0f;

		for (int contouri = 0; contouri < this->numContours; contouri++)
		{
			for (int curvei = 0; curvei < this->contours[contouri].numCurves; curvei++)
			{
				const Curve& curve = contours[contouri].curves[curvei];
				const Vec2& p0 = curve.p0;

				switch (curve.type)
				{
				case CurveType::Bezier3:
				{
					const Vec2& p1 = curve.as.bezier3.p1;
					const Vec2& p2 = curve.as.bezier3.p2;
					const Vec2& p3 = curve.as.bezier3.p3;
					float chordLength = CMath::length(p3 - p0);
					float controlNetLength = CMath::length(p1 - p0) + CMath::length(p2 - p1) + CMath::length(p3 - p2);
					float approxLength = (chordLength + controlNetLength) / 2.0f;
					approximatePerimeter += approxLength;
				}
				break;
				case CurveType::Bezier2:
				{
					const Vec2& p1 = curve.as.bezier2.p1;
					const Vec2& p2 = curve.as.bezier2.p2;
					float chordLength = CMath::length(p2 - p0);
					float controlNetLength = CMath::length(p1 - p0) + CMath::length(p2 - p1);
					float approxLength = (chordLength + controlNetLength) / 2.0f;
					approximatePerimeter += approxLength;
				}
				break;
				case CurveType::Line:
				{
					const Vec2& p1 = curve.as.line.p1;
					approximatePerimeter += CMath::length(p1 - p0);
				}
				break;
				}
			}
		}
	}

	void SvgObject::calculateBBox()
	{
		bbox.min.x = FLT_MAX;
		bbox.min.y = FLT_MAX;
		bbox.max.x = FLT_MIN;
		bbox.max.y = FLT_MIN;

		for (int contouri = 0; contouri < numContours; contouri++)
		{
			if (contours[contouri].numCurves > 0)
			{
				for (int curvei = 0; curvei < contours[contouri].numCurves; curvei++)
				{
					const Curve& curve = contours[contouri].curves[curvei];
					Vec2 p0 = Vec2{ curve.p0.x, curve.p0.y };

					switch (curve.type)
					{
					case CurveType::Bezier3:
					{
						BBox subBbox = CMath::bezier3BBox(p0, curve.as.bezier3.p1, curve.as.bezier3.p2, curve.as.bezier3.p3);
						bbox.min = CMath::min(bbox.min, subBbox.min);
						bbox.max = CMath::max(bbox.max, subBbox.max);
					}
					break;
					case CurveType::Bezier2:
					{
						BBox subBbox = CMath::bezier2BBox(p0, curve.as.bezier2.p1, curve.as.bezier2.p2);
						bbox.min = CMath::min(bbox.min, subBbox.min);
						bbox.max = CMath::max(bbox.max, subBbox.max);
					}
					break;
					case CurveType::Line:
					{
						BBox subBbox = CMath::bezier1BBox(p0, curve.as.line.p1);
						bbox.min = CMath::min(bbox.min, subBbox.min);
						bbox.max = CMath::max(bbox.max, subBbox.max);
					}
					break;
					default:
						g_logger_warning("Unknown curve type in render %d", (int)curve.type);
						break;
					}
				}
			}
		}
	}

	void SvgObject::render(NVGcontext* vg, const AnimObject* parent, const Vec2& offset, const Vec2& svgScale) const
	{
		renderCreateAnimation(vg, 1.01f, parent, offset, svgScale, false, false);
	}

	void SvgObject::renderCreateAnimation(NVGcontext* vg, float t, const AnimObject* parent, const Vec2& offset, const Vec2& svgScale, bool reverse, bool isSvgGroup) const
	{
		Vec2 svgTextureOffset = Vec2{
			(float)cacheCurrentPos.x + parent->strokeWidth * 0.5f,
			(float)cacheCurrentPos.y + parent->strokeWidth * 0.5f
		};

		// Check if the SVG cache needs to regenerate
		float svgTotalWidth = ((bbox.max.x - bbox.min.x) * svgScale.x) + parent->strokeWidth;
		float svgTotalHeight = ((bbox.max.y - bbox.min.y) * svgScale.y) + parent->strokeWidth;
		{
			float newRightX = svgTextureOffset.x + svgTotalWidth;
			if (newRightX >= svgCache.width)
			{
				// Move to the newline
				Svg::incrementCacheCurrentY();
			}

			float newBottomY = svgTextureOffset.y + svgTotalHeight;
			if (newBottomY >= svgCache.height)
			{
				Svg::growCache();
			}

			svgTextureOffset = Vec2{
				(float)cacheCurrentPos.x + parent->strokeWidth * 0.5f,
				(float)cacheCurrentPos.y + parent->strokeWidth * 0.5f
			};
		}

		// Render to the framebuffer then blit the framebuffer to the screen
		// with the appropriate transformations
		int32 lastFboId;
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &lastFboId);

		// First render to the cache
		svgCache.bind();
		glViewport(0, 0, svgCache.width, svgCache.height);

		// Reset the draw buffers to draw to FB_attachment_0
		GLenum compositeDrawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE };
		glDrawBuffers(3, compositeDrawBuffers);

		if (isSvgGroup)
		{
			svgTextureOffset.x += offset.x * svgScale.x;
			svgTextureOffset.y += offset.y * svgScale.y;
		}

		nvgBeginFrame(vg, svgCache.width, svgCache.height, 1.0f);
		renderCreateAnimation2D(vg, t, parent, svgTextureOffset, svgScale, reverse, this, isSvgGroup);
		nvgEndFrame(vg);

		// Then bind the previous fbo and blit it to the screen with
		// the appropriate transformations if it's not an svg group
		// SVG Groups get drawn in one draw call
		glBindFramebuffer(GL_FRAMEBUFFER, lastFboId);
		// Reset the draw buffers to draw to FB_attachment_0
		glDrawBuffers(3, compositeDrawBuffers);

		// Don't blit svg groups to a bunch of quads, they get rendered as one quad together
		if (isSvgGroup)
		{
			return;
		}

		// Subtract half stroke width to make sure it's getting the correct coords
		svgTextureOffset -= Vec2{ parent->strokeWidth * 0.5f, parent->strokeWidth * 0.5f };
		Vec2 cacheUvMin = Vec2{
			svgTextureOffset.x / (float)svgCache.width,
			1.0f - (svgTextureOffset.y / (float)svgCache.height) - (svgTotalHeight / (float)svgCache.height)
		};
		Vec2 cacheUvMax = cacheUvMin +
			Vec2{
				svgTotalWidth / (float)svgCache.width,
				svgTotalHeight / (float)svgCache.height
		};

		Svg::incrementCacheCurrentX(svgTotalWidth + cachePadding.x);
		Svg::checkLineHeight(svgTotalHeight);

		if (parent->is3D)
		{
			glm::mat4 transform = glm::identity<glm::mat4>();
			transform = glm::translate(
				transform,
				glm::vec3(
					parent->position.x + (offset.x * parent->scale.x),
					parent->position.y + (offset.y * parent->scale.y),
					parent->position.z
				)
			);
			transform = transform * glm::orientate4(glm::radians(glm::vec3(parent->rotation.x, parent->rotation.y, parent->rotation.z)));
			transform = glm::scale(transform, glm::vec3(parent->scale.x, parent->scale.y, parent->scale.z));

			Renderer::drawTexturedQuad3D(
				svgCache.getColorAttachment(0),
				Vec2{ svgTotalWidth * 0.01f, svgTotalHeight * 0.01f },
				cacheUvMin,
				cacheUvMax,
				transform,
				parent->isTransparent
			);
		}
		else
		{
			glm::mat4 transform = glm::identity<glm::mat4>();
			Vec2 cameraCenteredPos = Svg::orthoCamera->projectionSize / 2.0f - Svg::orthoCamera->position;
			transform = glm::translate(
				transform,
				glm::vec3(
					parent->position.x - cameraCenteredPos.x + (offset.x * parent->scale.x),
					parent->position.y - cameraCenteredPos.y + (offset.y * parent->scale.y),
					0.0f
				)
			);
			if (!CMath::compare(parent->rotation.z, 0.0f))
			{
				transform = glm::rotate(transform, parent->rotation.z, glm::vec3(0, 0, 1));
			}
			transform = glm::scale(transform, glm::vec3(parent->scale.x, parent->scale.y, parent->scale.z));

			Renderer::drawTexturedQuad(
				svgCache.getColorAttachment(0),
				Vec2{ svgTotalWidth, svgTotalHeight },
				cacheUvMin,
				cacheUvMax,
				transform
			);
		}
	}

	void SvgObject::free()
	{
		for (int contouri = 0; contouri < numContours; contouri++)
		{
			if (contours[contouri].curves)
			{
				g_memory_free(contours[contouri].curves);
			}
			contours[contouri].numCurves = 0;
			contours[contouri].maxCapacity = 0;
		}

		if (contours)
		{
			g_memory_free(contours);
		}

		numContours = 0;
		approximatePerimeter = 0.0f;
	}

	void SvgGroup::normalize()
	{
		Vec3 translation = Vec3{ viewbox.values[0], viewbox.values[1], 0.0f };

		calculateBBox();
		//for (int i = 0; i < numObjects; i++)
		//{
		//	SvgObject& obj = objects[i];
		//	Vec3& offset = objectOffsets[i];
		//	Vec2 absOffset = CMath::vector2From3(offset - translation);
		//	obj.normalize(bbox.min, bbox.max);
		//	offset.x = CMath::mapRange(Vec2{ bbox.min.x, bbox.max.x }, Vec2{ 0.0f, 1.0f }, absOffset.x);
		//	offset.y = CMath::mapRange(Vec2{ bbox.min.y, bbox.max.y }, Vec2{ 0.0f, 1.0f }, absOffset.y);
		//	obj.calculateSvgSize();
		//}
		//viewbox.values[0] = CMath::mapRange(Vec2{ bbox.min.x, bbox.max.x }, Vec2{ 0.0f, 1.0f }, viewbox.values[0]);
		//viewbox.values[1] = CMath::mapRange(Vec2{ bbox.min.x, bbox.max.x }, Vec2{ 0.0f, 1.0f }, viewbox.values[1]);
		//viewbox.values[2] = CMath::mapRange(Vec2{ bbox.min.x, bbox.max.x }, Vec2{ 0.0f, 1.0f }, viewbox.values[2]);
		//viewbox.values[3] = CMath::mapRange(Vec2{ bbox.min.x, bbox.max.x }, Vec2{ 0.0f, 1.0f }, viewbox.values[3]);
		//calculateBBox();
	}

	void SvgGroup::calculateBBox()
	{
		Vec2 translation = Vec2{ viewbox.values[0], viewbox.values[1] };
		bbox.min = Vec2{ FLT_MAX, FLT_MAX };
		bbox.max = Vec2{ FLT_MIN, FLT_MIN };

		for (int i = 0; i < numObjects; i++)
		{
			SvgObject& obj = objects[i];
			const Vec2& offset = objectOffsets[i];

			Vec2 absOffset = offset - translation;
			obj.calculateBBox();
			bbox.min = CMath::min(obj.bbox.min + absOffset, bbox.min);
			bbox.max = CMath::max(obj.bbox.max + absOffset, bbox.max);
		}
	}

	void SvgGroup::render(NVGcontext* vg, AnimObject* parent, const Vec2& svgScale) const
	{
		renderCreateAnimation(vg, 1.01f, parent, svgScale, false);
	}

	void SvgGroup::renderCreateAnimation(NVGcontext* vg, float t, AnimObject* parent, const Vec2& svgScale, bool reverse) const
	{
		Vec2 translation = Vec2{ viewbox.values[0], viewbox.values[1] };
		Vec2 bboxOffset = Vec2{ bbox.min.x, bbox.min.y };

		// TODO: Offload all this stuff into some sort of TexturePacker data structure
		{
			Vec2 svgTextureOffset = Vec2{
				(float)cacheCurrentPos.x + parent->strokeWidth * 0.5f,
				(float)cacheCurrentPos.y + parent->strokeWidth * 0.5f
			};

			// Check if the SVG cache needs to regenerate
			float svgTotalWidth = ((bbox.max.x - bbox.min.x) * svgScale.x) + parent->strokeWidth;
			float svgTotalHeight = ((bbox.max.y - bbox.min.y) * svgScale.y) + parent->strokeWidth;
			{
				float newRightX = svgTextureOffset.x + svgTotalWidth;
				if (newRightX >= svgCache.width)
				{
					// Move to the newline
					Svg::incrementCacheCurrentY();
				}

				float newBottomY = svgTextureOffset.y + svgTotalHeight;
				if (newBottomY >= svgCache.height)
				{
					Svg::growCache();
				}
			}
		}

		float numberObjectsToDraw = t * (float)numObjects;
		constexpr float numObjectsToLag = 2.0f;
		float numObjectsDrawn = 0.0f;
		for (int i = 0; i < numObjects; i++)
		{
			const SvgObject& obj = objects[i];
			const Vec2& offset = objectOffsets[i];

			float denominator = i == numObjects - 1 ? 1.0f : numObjectsToLag;
			float percentOfLetterToDraw = (numberObjectsToDraw - numObjectsDrawn) / denominator;
			Vec2 absOffset = offset - translation - bboxOffset;
			obj.renderCreateAnimation(vg, percentOfLetterToDraw, parent, absOffset, svgScale, reverse, true);
			numObjectsDrawn += 1.0f;

			if (numObjectsDrawn >= numberObjectsToDraw)
			{
				break;
			}
		}

		Vec2 svgTextureOffset = Vec2{
		(float)cacheCurrentPos.x + parent->strokeWidth * 0.5f,
		(float)cacheCurrentPos.y + parent->strokeWidth * 0.5f
		};
		float svgTotalWidth = ((bbox.max.x - bbox.min.x) * svgScale.x) + parent->strokeWidth;
		float svgTotalHeight = ((bbox.max.y - bbox.min.y) * svgScale.y) + parent->strokeWidth;
		Vec2 cacheUvMin = Vec2{
			svgTextureOffset.x / (float)svgCache.width,
			1.0f - (svgTextureOffset.y / (float)svgCache.height) - (svgTotalHeight / (float)svgCache.height)
		};
		Vec2 cacheUvMax = cacheUvMin +
			Vec2{
				svgTotalWidth / (float)svgCache.width,
				svgTotalHeight / (float)svgCache.height
		};

		if (parent->drawDebugBoxes)
		{
			// Render to the framebuffer then blit the framebuffer to the screen
			// with the appropriate transformations
			int32 lastFboId;
			glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &lastFboId);

			// First render to the cache
			svgCache.bind();
			glViewport(0, 0, svgCache.width, svgCache.height);

			// Reset the draw buffers to draw to FB_attachment_0
			GLenum compositeDrawBuffers[] = { GL_COLOR_ATTACHMENT0, GL_NONE, GL_NONE };
			glDrawBuffers(3, compositeDrawBuffers);

			nvgBeginFrame(vg, svgCache.width, svgCache.height, 1.0f);

			float strokeWidthCorrectionPos = cachePadding.x * 0.5f;
			float strokeWidthCorrectionNeg = -cachePadding.x;
			nvgBeginPath(vg);
			nvgStrokeColor(vg, nvgRGBA(0, 255, 255, 255));
			nvgStrokeWidth(vg, cachePadding.x);
			nvgMoveTo(vg,
				cacheUvMin.x * svgCache.width + strokeWidthCorrectionPos,
				(1.0f - cacheUvMax.y) * svgCache.height + strokeWidthCorrectionPos
			);
			nvgRect(vg,
				cacheUvMin.x * svgCache.width + strokeWidthCorrectionPos,
				(1.0f - cacheUvMax.y) * svgCache.height + strokeWidthCorrectionPos,
				(cacheUvMax.x - cacheUvMin.x) * svgCache.width + strokeWidthCorrectionNeg,
				(cacheUvMax.y - cacheUvMin.y) * svgCache.height + strokeWidthCorrectionNeg
			);
			nvgClosePath(vg);
			nvgStroke(vg);
			nvgEndFrame(vg);

			// Then bind the previous fbo and blit it to the screen with
			// the appropriate transformations
			glBindFramebuffer(GL_FRAMEBUFFER, lastFboId);

			// Reset the draw buffers to draw to FB_attachment_0
			glDrawBuffers(3, compositeDrawBuffers);
		}

		// Then blit the SVG group to the screen
		if (parent->is3D)
		{
			glm::mat4 transform = glm::identity<glm::mat4>();
			transform = glm::translate(
				transform,
				glm::vec3(
					parent->position.x,
					parent->position.y,
					parent->position.z
				)
			);
			transform = transform * glm::orientate4(glm::radians(glm::vec3(parent->rotation.x, parent->rotation.y, parent->rotation.z)));
			transform = glm::scale(transform, glm::vec3(parent->scale.x, parent->scale.y, parent->scale.z));

			Renderer::drawTexturedQuad3D(
				svgCache.getColorAttachment(0),
				Vec2{ svgTotalWidth * 0.01f, svgTotalHeight * 0.01f },
				cacheUvMin,
				cacheUvMax,
				transform,
				parent->isTransparent
			);
		}
		else
		{
			glm::mat4 transform = glm::identity<glm::mat4>();
			Vec2 cameraCenteredPos = Svg::orthoCamera->projectionSize / 2.0f - Svg::orthoCamera->position;
			transform = glm::translate(
				transform,
				glm::vec3(
					parent->position.x - cameraCenteredPos.x,
					parent->position.y - cameraCenteredPos.y,
					0.0f
				)
			);
			if (!CMath::compare(parent->rotation.z, 0.0f))
			{
				transform = glm::rotate(transform, parent->rotation.z, glm::vec3(0, 0, 1));
			}
			transform = glm::scale(transform, glm::vec3(parent->scale.x, parent->scale.y, parent->scale.z));

			Renderer::drawTexturedQuad(
				svgCache.getColorAttachment(0),
				Vec2{ svgTotalWidth, svgTotalHeight },
				cacheUvMin,
				cacheUvMax,
				transform
			);
		}

		Svg::incrementCacheCurrentX(((bbox.max.x - bbox.min.x) * svgScale.x) + parent->strokeWidth + cachePadding.x);
		Svg::checkLineHeight(((bbox.max.y - bbox.min.y) * svgScale.y) + parent->strokeWidth);
	}

	void SvgGroup::free()
	{
		for (int i = 0; i < numUniqueObjects; i++)
		{
			if (uniqueObjectNames && uniqueObjectNames[i])
			{
				g_memory_free(uniqueObjectNames[i]);
				uniqueObjectNames[i] = nullptr;
			}

			if (uniqueObjects)
			{
				uniqueObjects[i].free();
			}
		}

		if (uniqueObjectNames)
		{
			g_memory_free(uniqueObjectNames);
		}

		if (uniqueObjects)
		{
			g_memory_free(uniqueObjects);
		}

		if (objects)
		{
			g_memory_free(objects);
		}

		if (objectOffsets)
		{
			g_memory_free(objectOffsets);
		}

		numUniqueObjects = 0;
		numObjects = 0;
		objectOffsets = nullptr;
		objects = nullptr;
		uniqueObjects = nullptr;
		uniqueObjectNames = nullptr;
		viewbox = Vec4{ 0, 0, 0, 0 };
	}

	// ------------------- Svg Object Internal functions -------------------
	static void renderCreateAnimation2D(NVGcontext* vg, float t, const AnimObject* parent, const Vec2& textureOffset, const Vec2& svgScale, bool reverse, const SvgObject* obj, bool isSvgGroup)
	{
		if (reverse)
		{
			t = 1.0f - t;
		}

		// Start the fade in after 80% of the svg object is drawn
		constexpr float fadeInStart = 0.8f;
		float lengthToDraw = t * (float)obj->approximatePerimeter;
		float amountToFadeIn = ((t - fadeInStart) / (1.0f - fadeInStart));
		float percentToFadeIn = glm::max(glm::min(amountToFadeIn, 1.0f), 0.0f);

		// Instead of translating, we'll map every coordinate from the SVG min-max range to
		// the preferred coordinate range
		//nvgTranslate(vg, textureOffset.x, textureOffset.y);
		Vec2 scaledBboxMin = obj->bbox.min;
		scaledBboxMin.x *= svgScale.x;
		scaledBboxMin.y *= svgScale.y;
		// TODO: This may cause issues with SVGs that have negative values
		// Comment out this if-statement if it does
		if (!isSvgGroup)
		{
			scaledBboxMin = CMath::max(scaledBboxMin, Vec2{ 0, 0 });
		}
		Vec2 minCoord = textureOffset + scaledBboxMin;
		Vec2 bboxSize = (obj->bbox.max - obj->bbox.min);
		bboxSize.x *= svgScale.x;
		bboxSize.y *= svgScale.y;
		Vec2 maxCoord = minCoord + bboxSize;

		Vec2 inXRange = Vec2{ obj->bbox.min.x * svgScale.x, obj->bbox.max.x * svgScale.x };
		Vec2 inYRange = Vec2{ obj->bbox.min.y * svgScale.y, obj->bbox.max.y * svgScale.y };
		Vec2 outXRange = Vec2{ minCoord.x, maxCoord.x };
		Vec2 outYRange = Vec2{ minCoord.y, maxCoord.y };

		if (lengthToDraw > 0)
		{
			float lengthDrawn = 0.0f;
			for (int contouri = 0; contouri < obj->numContours; contouri++)
			{
				if (obj->contours[contouri].numCurves > 0)
				{
					nvgBeginPath(vg);

					// Fade the stroke out as the svg fades in
					const glm::u8vec4& strokeColor = parent->strokeColor;
					if (glm::epsilonEqual(parent->strokeWidth, 0.0f, 0.01f))
					{
						nvgStrokeColor(vg, nvgRGBA(strokeColor.r, strokeColor.g, strokeColor.b, (unsigned char)((float)strokeColor.a * (1.0f - percentToFadeIn))));
						nvgStrokeWidth(vg, 5.0f);
					}
					else
					{
						nvgStrokeColor(vg, nvgRGBA(strokeColor.r, strokeColor.g, strokeColor.b, strokeColor.a));
						nvgStrokeWidth(vg, parent->strokeWidth);
					}

					{
						Vec2 p0 = obj->contours[contouri].curves[0].p0;
						p0.x *= svgScale.x;
						p0.y *= svgScale.y;
						p0.x = CMath::mapRange(inXRange, outXRange, p0.x);
						p0.y = CMath::mapRange(inYRange, outYRange, p0.y);

						nvgMoveTo(vg,
							p0.x,
							p0.y
						);
					}

					for (int curvei = 0; curvei < obj->contours[contouri].numCurves; curvei++)
					{
						float lengthLeft = lengthToDraw - lengthDrawn;
						if (lengthLeft < 0.0f)
						{
							break;
						}

						const Curve& curve = obj->contours[contouri].curves[curvei];
						Vec2 p0 = curve.p0;

						if (curve.moveToP0)
						{
							Vec2 transformedP0 = p0;
							transformedP0.x *= svgScale.x;
							transformedP0.y *= svgScale.y;
							transformedP0.x = CMath::mapRange(inXRange, outXRange, transformedP0.x);
							transformedP0.y = CMath::mapRange(inYRange, outYRange, transformedP0.y);
							nvgMoveTo(vg, transformedP0.x, transformedP0.y);
						}

						switch (curve.type)
						{
						case CurveType::Bezier3:
						{
							Vec2 p1 = curve.as.bezier3.p1;
							Vec2 p2 = curve.as.bezier3.p2;
							Vec2 p3 = curve.as.bezier3.p3;

							float chordLength = CMath::length(p3 - p0);
							float controlNetLength = CMath::length(p1 - p0) + CMath::length(p2 - p1) + CMath::length(p3 - p2);
							float approxLength = (chordLength + controlNetLength) / 2.0f;
							lengthDrawn += approxLength;

							if (lengthLeft < approxLength)
							{
								// Interpolate the curve
								float percentOfCurveToDraw = lengthLeft / approxLength;

								// Taken from https://stackoverflow.com/questions/878862/drawing-part-of-a-b�zier-curve-by-reusing-a-basic-b�zier-curve-function
								float t0 = 0.0f;
								float t1 = percentOfCurveToDraw;
								float u0 = 1.0f;
								float u1 = (1.0f - t1);

								Vec2 q0 = ((u0 * u0 * u0) * p0) +
									((t0 * u0 * u0 + u0 * t0 * u0 + u0 * u0 * t0) * p1) +
									((t0 * t0 * u0 + u0 * t0 * t0 + t0 * u0 * t0) * p2) +
									((t0 * t0 * t0) * p3);
								Vec2 q1 = ((u0 * u0 * u1) * p0) +
									((t0 * u0 * u1 + u0 * t0 * u1 + u0 * u0 * t1) * p1) +
									((t0 * t0 * u1 + u0 * t0 * t1 + t0 * u0 * t1) * p2) +
									((t0 * t0 * t1) * p3);
								Vec2 q2 = ((u0 * u1 * u1) * p0) +
									((t0 * u1 * u1 + u0 * t1 * u1 + u0 * u1 * t1) * p1) +
									((t0 * t1 * u1 + u0 * t1 * t1 + t0 * u1 * t1) * p2) +
									((t0 * t1 * t1) * p3);
								Vec2 q3 = ((u1 * u1 * u1) * p0) +
									((t1 * u1 * u1 + u1 * t1 * u1 + u1 * u1 * t1) * p1) +
									((t1 * t1 * u1 + u1 * t1 * t1 + t1 * u1 * t1) * p2) +
									((t1 * t1 * t1) * p3);

								p1 = q1;
								p2 = q2;
								p3 = q3;
							}

							p1.x *= svgScale.x;
							p1.y *= svgScale.y;
							p1.x = CMath::mapRange(inXRange, outXRange, p1.x);
							p1.y = CMath::mapRange(inYRange, outYRange, p1.y);

							p2.x *= svgScale.x;
							p2.y *= svgScale.y;
							p2.x = CMath::mapRange(inXRange, outXRange, p2.x);
							p2.y = CMath::mapRange(inYRange, outYRange, p2.y);

							p3.x *= svgScale.x;
							p3.y *= svgScale.y;
							p3.x = CMath::mapRange(inXRange, outXRange, p3.x);
							p3.y = CMath::mapRange(inYRange, outYRange, p3.y);

							nvgBezierTo(
								vg,
								p1.x, p1.y,
								p2.x, p2.y,
								p3.x, p3.y
							);
						}
						break;
						case CurveType::Bezier2:
						{
							Vec2 p1 = curve.as.bezier2.p1;
							Vec2 p2 = curve.as.bezier2.p1;
							Vec2 p3 = curve.as.bezier2.p2;

							// Degree elevated quadratic bezier curve
							Vec2 pr0 = p0;
							Vec2 pr1 = (1.0f / 3.0f) * p0 + (2.0f / 3.0f) * p1;
							Vec2 pr2 = (2.0f / 3.0f) * p1 + (1.0f / 3.0f) * p2;
							Vec2 pr3 = p3;

							float chordLength = CMath::length(pr3 - pr0);
							float controlNetLength = CMath::length(pr1 - pr0) + CMath::length(pr2 - pr1) + CMath::length(pr3 - pr2);
							float approxLength = (chordLength + controlNetLength) / 2.0f;
							lengthDrawn += approxLength;

							if (lengthLeft < approxLength)
							{
								// Interpolate the curve
								float percentOfCurveToDraw = lengthLeft / approxLength;

								p1 = (pr1 - pr0) * percentOfCurveToDraw + pr0;
								p2 = (pr2 - pr1) * percentOfCurveToDraw + pr1;
								p3 = (pr3 - pr2) * percentOfCurveToDraw + pr2;

								// Taken from https://stackoverflow.com/questions/878862/drawing-part-of-a-b�zier-curve-by-reusing-a-basic-b�zier-curve-function
								float t0 = 0.0f;
								float t1 = percentOfCurveToDraw;
								float u0 = 1.0f;
								float u1 = (1.0f - t1);

								Vec2 q0 = ((u0 * u0 * u0) * p0) +
									((t0 * u0 * u0 + u0 * t0 * u0 + u0 * u0 * t0) * p1) +
									((t0 * t0 * u0 + u0 * t0 * t0 + t0 * u0 * t0) * p2) +
									((t0 * t0 * t0) * p3);
								Vec2 q1 = ((u0 * u0 * u1) * p0) +
									((t0 * u0 * u1 + u0 * t0 * u1 + u0 * u0 * t1) * p1) +
									((t0 * t0 * u1 + u0 * t0 * t1 + t0 * u0 * t1) * p2) +
									((t0 * t0 * t1) * p3);
								Vec2 q2 = ((u0 * u1 * u1) * p0) +
									((t0 * u1 * u1 + u0 * t1 * u1 + u0 * u1 * t1) * p1) +
									((t0 * t1 * u1 + u0 * t1 * t1 + t0 * u1 * t1) * p2) +
									((t0 * t1 * t1) * p3);
								Vec2 q3 = ((u1 * u1 * u1) * p0) +
									((t1 * u1 * u1 + u1 * t1 * u1 + u1 * u1 * t1) * p1) +
									((t1 * t1 * u1 + u1 * t1 * t1 + t1 * u1 * t1) * p2) +
									((t1 * t1 * t1) * p3);

								pr1 = q1;
								pr2 = q2;
								pr3 = q3;
							}

							pr1.x *= svgScale.x;
							pr1.y *= svgScale.y;
							pr1.x = CMath::mapRange(inXRange, outXRange, pr1.x);
							pr1.y = CMath::mapRange(inYRange, outYRange, pr1.y);

							pr2.x *= svgScale.x;
							pr2.y *= svgScale.y;
							pr2.x = CMath::mapRange(inXRange, outXRange, pr2.x);
							pr2.y = CMath::mapRange(inYRange, outYRange, pr2.y);

							pr3.x *= svgScale.x;
							pr3.y *= svgScale.y;
							pr3.x = CMath::mapRange(inXRange, outXRange, pr3.x);
							pr3.y = CMath::mapRange(inYRange, outYRange, pr3.y);

							nvgBezierTo(
								vg,
								pr1.x, pr1.y,
								pr2.x, pr2.y,
								pr3.x, pr3.y
							);
						}
						break;
						case CurveType::Line:
						{
							Vec2 p1 = curve.as.line.p1;
							float curveLength = CMath::length(p1 - p0);
							lengthDrawn += curveLength;

							if (lengthLeft < curveLength)
							{
								float percentOfCurveToDraw = lengthLeft / curveLength;
								p1 = (p1 - p0) * percentOfCurveToDraw + p0;
							}

							p1.x *= svgScale.x;
							p1.y *= svgScale.y;
							p1.x = CMath::mapRange(inXRange, outXRange, p1.x);
							p1.y = CMath::mapRange(inYRange, outYRange, p1.y);

							nvgLineTo(vg, p1.x, p1.y);
						}
						break;
						default:
							g_logger_warning("Unknown curve type in render %d", (int)curve.type);
							break;
						}
					}
				}

				nvgStroke(vg);

				if (lengthDrawn > lengthToDraw)
				{
					break;
				}
			}
		}

		if (amountToFadeIn > 0)
		{
			for (int contouri = 0; contouri < obj->numContours; contouri++)
			{
				if (obj->contours[contouri].numCurves > 0)
				{
					// Begin path
					nvgBeginPath(vg);

					{
						Vec2 p0 = obj->contours[contouri].curves[0].p0;
						p0.x *= svgScale.x;
						p0.y *= svgScale.y;
						p0.x = CMath::mapRange(inXRange, outXRange, p0.x);
						p0.y = CMath::mapRange(inYRange, outYRange, p0.y);

						nvgMoveTo(vg,
							p0.x,
							p0.y
						);
					}

					for (int curvei = 0; curvei < obj->contours[contouri].numCurves; curvei++)
					{
						const Curve& curve = obj->contours[contouri].curves[curvei];
						Vec2 p0 = curve.p0;

						if (curvei != 0 && curve.moveToP0)
						{
							Vec2 transformedP0 = p0;
							transformedP0.x *= svgScale.x;
							transformedP0.y *= svgScale.y;
							transformedP0.x = CMath::mapRange(inXRange, outXRange, transformedP0.x);
							transformedP0.y = CMath::mapRange(inYRange, outYRange, transformedP0.y);
							nvgMoveTo(vg, transformedP0.x, transformedP0.y);
							// TODO: Does this work consistently???
							nvgPathWinding(vg, NVG_HOLE);
						}

						switch (curve.type)
						{
						case CurveType::Bezier3:
						{
							Vec2 p1 = curve.as.bezier3.p1;
							Vec2 p2 = curve.as.bezier3.p2;
							Vec2 p3 = curve.as.bezier3.p3;

							p1.x *= svgScale.x;
							p1.y *= svgScale.y;
							p1.x = CMath::mapRange(inXRange, outXRange, p1.x);
							p1.y = CMath::mapRange(inYRange, outYRange, p1.y);

							p2.x *= svgScale.x;
							p2.y *= svgScale.y;
							p2.x = CMath::mapRange(inXRange, outXRange, p2.x);
							p2.y = CMath::mapRange(inYRange, outYRange, p2.y);

							p3.x *= svgScale.x;
							p3.y *= svgScale.y;
							p3.x = CMath::mapRange(inXRange, outXRange, p3.x);
							p3.y = CMath::mapRange(inYRange, outYRange, p3.y);

							nvgBezierTo(
								vg,
								p1.x, p1.y,
								p2.x, p2.y,
								p3.x, p3.y
							);
						}
						break;
						case CurveType::Bezier2:
						{
							Vec2 p1 = curve.as.bezier2.p1;
							Vec2 p2 = curve.as.bezier2.p1;
							Vec2 p3 = curve.as.bezier2.p2;

							// Degree elevated quadratic bezier curve
							Vec2 pr0 = p0;
							Vec2 pr1 = (1.0f / 3.0f) * p0 + (2.0f / 3.0f) * p1;
							Vec2 pr2 = (2.0f / 3.0f) * p1 + (1.0f / 3.0f) * p2;
							Vec2 pr3 = p3;

							pr1.x *= svgScale.x;
							pr1.y *= svgScale.y;
							pr1.x = CMath::mapRange(inXRange, outXRange, pr1.x);
							pr1.y = CMath::mapRange(inYRange, outYRange, pr1.y);

							pr2.x *= svgScale.x;
							pr2.y *= svgScale.y;
							pr2.x = CMath::mapRange(inXRange, outXRange, pr2.x);
							pr2.y = CMath::mapRange(inYRange, outYRange, pr2.y);

							pr3.x *= svgScale.x;
							pr3.y *= svgScale.y;
							pr3.x = CMath::mapRange(inXRange, outXRange, pr3.x);
							pr3.y = CMath::mapRange(inYRange, outYRange, pr3.y);

							nvgBezierTo(
								vg,
								pr1.x, pr1.y,
								pr2.x, pr2.y,
								pr3.x, pr3.y
							);
						}
						break;
						case CurveType::Line:
						{
							Vec2 p1 = curve.as.line.p1;

							p1.x *= svgScale.x;
							p1.y *= svgScale.y;
							p1.x = CMath::mapRange(inXRange, outXRange, p1.x);
							p1.y = CMath::mapRange(inYRange, outYRange, p1.y);

							nvgLineTo(vg, p1.x, p1.y);
						}
						break;
						default:
							g_logger_warning("Unknown curve type in render %d", (int)curve.type);
							break;
						}
					}

					// Fill path
					const glm::u8vec4& fillColor = parent->fillColor;
					nvgFillColor(vg, nvgRGBA(fillColor.r, fillColor.g, fillColor.b, (unsigned char)(fillColor.a * percentToFadeIn)));
					nvgFill(vg);

					if (obj->contours[contouri].isHole)
					{
						nvgPathWinding(vg, NVG_HOLE);
					}
				}
			}
		}

		if (parent->drawDebugBoxes)
		{
			float debugStrokeWidth = cachePadding.x;
			float strokeWidthCorrectionPos = debugStrokeWidth * 0.5f;
			float strokeWidthCorrectionNeg = -debugStrokeWidth;

			if (parent->drawCurveDebugBoxes)
			{
				for (int contouri = 0; contouri < obj->numContours; contouri++)
				{
					if (obj->contours[contouri].numCurves > 0)
					{
						for (int curvei = 0; curvei < obj->contours[contouri].numCurves; curvei++)
						{
							const Curve& curve = obj->contours[contouri].curves[curvei];
							Vec2 p0 = curve.p0;
							p0.x *= svgScale.x;
							p0.y *= svgScale.y;
							p0.x = CMath::mapRange(inXRange, outXRange, p0.x);
							p0.y = CMath::mapRange(inYRange, outYRange, p0.y);

							switch (curve.type)
							{
							case CurveType::Bezier3:
							{
								Vec2 p1 = curve.as.bezier3.p1;
								Vec2 p2 = curve.as.bezier3.p2;
								Vec2 p3 = curve.as.bezier3.p3;

								p1.x *= svgScale.x;
								p1.y *= svgScale.y;
								p1.x = CMath::mapRange(inXRange, outXRange, p1.x);
								p1.y = CMath::mapRange(inYRange, outYRange, p1.y);

								p2.x *= svgScale.x;
								p2.y *= svgScale.y;
								p2.x = CMath::mapRange(inXRange, outXRange, p2.x);
								p2.y = CMath::mapRange(inYRange, outYRange, p2.y);

								p3.x *= svgScale.x;
								p3.y *= svgScale.y;
								p3.x = CMath::mapRange(inXRange, outXRange, p3.x);
								p3.y = CMath::mapRange(inYRange, outYRange, p3.y);

								BBox bbox = CMath::bezier3BBox(p0, p1, p2, p3);

								nvgBeginPath(vg);
								nvgStrokeWidth(vg, debugStrokeWidth);
								nvgStrokeColor(vg, nvgRGB(255, 0, 0));
								nvgFillColor(vg, nvgRGBA(0, 0, 0, 0));
								nvgMoveTo(vg,
									bbox.min.x + strokeWidthCorrectionPos,
									bbox.min.y + strokeWidthCorrectionPos
								);
								nvgRect(vg,
									bbox.min.x + strokeWidthCorrectionPos,
									bbox.min.y + strokeWidthCorrectionPos,
									bbox.max.x - bbox.min.x + strokeWidthCorrectionNeg,
									bbox.max.y - bbox.min.y + strokeWidthCorrectionNeg
								);
								nvgClosePath(vg);
								nvgStroke(vg);
							}
							break;
							case CurveType::Bezier2:
							{
								Vec2 p1 = curve.as.bezier2.p1;
								Vec2 p2 = curve.as.bezier2.p1;
								Vec2 p3 = curve.as.bezier2.p2;

								p1.x *= svgScale.x;
								p1.y *= svgScale.y;
								p1.x = CMath::mapRange(inXRange, outXRange, p1.x);
								p1.y = CMath::mapRange(inYRange, outYRange, p1.y);

								p2.x *= svgScale.x;
								p2.y *= svgScale.y;
								p2.x = CMath::mapRange(inXRange, outXRange, p2.x);
								p2.y = CMath::mapRange(inYRange, outYRange, p2.y);

								p3.x *= svgScale.x;
								p3.y *= svgScale.y;
								p3.x = CMath::mapRange(inXRange, outXRange, p3.x);
								p3.y = CMath::mapRange(inYRange, outYRange, p3.y);

								// Degree elevated quadratic bezier curve
								Vec2 pr0 = p0;
								Vec2 pr1 = (1.0f / 3.0f) * p0 + (2.0f / 3.0f) * p1;
								Vec2 pr2 = (2.0f / 3.0f) * p1 + (1.0f / 3.0f) * p2;
								Vec2 pr3 = p3;

								BBox bbox = CMath::bezier3BBox(pr0, pr1, pr2, pr3);

								nvgBeginPath(vg);
								nvgStrokeWidth(vg, debugStrokeWidth);
								nvgStrokeColor(vg, nvgRGB(255, 0, 0));
								nvgFillColor(vg, nvgRGBA(0, 0, 0, 0));
								nvgMoveTo(vg,
									bbox.min.x + strokeWidthCorrectionPos,
									bbox.min.y + strokeWidthCorrectionPos
								);
								nvgRect(vg,
									bbox.min.x + strokeWidthCorrectionPos,
									bbox.min.y + strokeWidthCorrectionPos,
									bbox.max.x - bbox.min.x + strokeWidthCorrectionNeg,
									bbox.max.y - bbox.min.y + strokeWidthCorrectionNeg
								);
								nvgClosePath(vg);
								nvgStroke(vg);
							}
							break;
							case CurveType::Line:
							{
								Vec2 p1 = curve.as.line.p1;

								p1.x *= svgScale.x;
								p1.y *= svgScale.y;
								p1.x = CMath::mapRange(inXRange, outXRange, p1.x);
								p1.y = CMath::mapRange(inYRange, outYRange, p1.y);

								BBox bbox = CMath::bezier1BBox(p0, p1);

								nvgBeginPath(vg);
								nvgStrokeWidth(vg, debugStrokeWidth);
								nvgStrokeColor(vg, nvgRGB(255, 0, 0));
								nvgFillColor(vg, nvgRGBA(0, 0, 0, 0));
								nvgMoveTo(vg,
									bbox.min.x + strokeWidthCorrectionPos,
									bbox.min.y + strokeWidthCorrectionPos
								);
								nvgRect(vg,
									bbox.min.x + strokeWidthCorrectionPos,
									bbox.min.y + strokeWidthCorrectionPos,
									bbox.max.x - bbox.min.x + strokeWidthCorrectionNeg,
									bbox.max.y - bbox.min.y + strokeWidthCorrectionNeg
								);
								nvgClosePath(vg);
								nvgStroke(vg);
							}
							break;
							default:
								g_logger_warning("Unknown curve type in render %d", (int)curve.type);
								break;
							}
						}
					}
				}
			}

			nvgBeginPath(vg);
			nvgStrokeWidth(vg, debugStrokeWidth);
			nvgStrokeColor(vg, nvgRGB(0, 255, 0));
			nvgFillColor(vg, nvgRGBA(0, 0, 0, 0));
			nvgMoveTo(vg,
				scaledBboxMin.x + textureOffset.x - (parent->strokeWidth * 0.5f) + strokeWidthCorrectionPos,
				scaledBboxMin.y + textureOffset.y - (parent->strokeWidth * 0.5f) + strokeWidthCorrectionPos
			);
			nvgRect(vg,
				scaledBboxMin.x + textureOffset.x - (parent->strokeWidth * 0.5f) + strokeWidthCorrectionPos,
				scaledBboxMin.y + textureOffset.y - (parent->strokeWidth * 0.5f) + strokeWidthCorrectionPos,
				bboxSize.x + parent->strokeWidth + strokeWidthCorrectionNeg,
				bboxSize.y + parent->strokeWidth + strokeWidthCorrectionNeg
			);
			nvgStroke(vg);
			nvgClosePath(vg);
		}

		nvgResetTransform(vg);
	}
}