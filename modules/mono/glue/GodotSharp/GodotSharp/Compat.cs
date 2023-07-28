// This file contains methods that existed in a previous version of Godot in ClassDB but were removed
// or their method signature has changed so they are no longer generated by bindings_generator.
// These methods are provided to avoid breaking binary compatibility.

using System;
using System.ComponentModel;

namespace Godot;

partial class AnimationNode
{
    /// <inheritdoc cref="BlendInput(int, double, bool, bool, float, FilterAction, bool, bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public double BlendInput(int inputIndex, double time, bool seek, bool isExternalSeeking, float blend, FilterAction filter, bool sync)
    {
        return BlendInput(inputIndex, time, seek, isExternalSeeking, blend, filter, sync, testOnly: false);
    }

    /// <inheritdoc cref="BlendNode(StringName, AnimationNode, double, bool, bool, float, FilterAction, bool, bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public double BlendNode(StringName name, AnimationNode node, double time, bool seek, bool isExternalSeeking, float blend, FilterAction filter, bool sync)
    {
        return BlendNode(name, node, time, seek, isExternalSeeking, blend, filter, sync, testOnly: false);
    }
}

partial class CodeEdit
{
    /// <inheritdoc cref="AddCodeCompletionOption(CodeCompletionKind, string, string, Nullable{Color}, Resource, Nullable{Variant})"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void AddCodeCompletionOption(CodeCompletionKind type, string displayText, string insertText, Nullable<Color> textColor, Resource icon, Nullable<Variant> value)
    {
        AddCodeCompletionOption(type, displayText, insertText, textColor, icon, value, location: 1024);
    }
}

partial class Geometry3D
{
    /// <inheritdoc cref="SegmentIntersectsConvex(Vector3, Vector3, Collections.Array{Plane})"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public static Vector3[] SegmentIntersectsConvex(Vector3 from, Vector3 to, Godot.Collections.Array planes)
    {
        return SegmentIntersectsConvex(from, to, new Godot.Collections.Array<Plane>(planes));
    }
}

partial class MeshInstance3D
{
    /// <inheritdoc cref="CreateMultipleConvexCollisions(MeshConvexDecompositionSettings)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void CreateMultipleConvexCollisions()
    {
        CreateMultipleConvexCollisions(settings: null);
    }
}

partial class Node3D
{
    /// <inheritdoc cref="LookAt(Vector3, Nullable{Vector3}, bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void LookAt(Vector3 target, Nullable<Vector3> up)
    {
        LookAt(target, up, useModelFront: false);
    }

    /// <inheritdoc cref="LookAtFromPosition(Vector3, Vector3, Nullable{Vector3}, bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void LookAtFromPosition(Vector3 position, Vector3 target, Nullable<Vector3> up)
    {
        LookAtFromPosition(position, target, up, useModelFront: false);
    }
}

partial class RenderingDevice
{
    /// <inheritdoc cref="DrawListBegin(Rid, InitialAction, FinalAction, InitialAction, FinalAction, Color[], float, uint, Nullable{Rect2}, Godot.Collections.Array{Rid})"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public long DrawListBegin(Rid framebuffer, InitialAction initialColorAction, FinalAction finalColorAction, InitialAction initialDepthAction, FinalAction finalDepthAction, Color[] clearColorValues, float clearDepth, uint clearStencil, Nullable<Rect2> region, Godot.Collections.Array storageTextures)
    {
        return DrawListBegin(framebuffer, initialColorAction, finalColorAction, initialDepthAction, finalDepthAction, clearColorValues, clearDepth, clearStencil, region, new Godot.Collections.Array<Rid>(storageTextures));
    }
}

partial class RichTextLabel
{
    /// <inheritdoc cref="PushList(int, ListType, bool, string)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void PushList(int level, ListType type, bool capitalize)
    {
        PushList(level, type, capitalize, bullet: "•");
    }

    /// <inheritdoc cref="PushParagraph(HorizontalAlignment, TextDirection, string, TextServer.StructuredTextParser, TextServer.JustificationFlag, float[])"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void PushParagraph(HorizontalAlignment alignment, TextDirection baseDirection, string language, TextServer.StructuredTextParser stParser)
    {
        PushParagraph(alignment, baseDirection, language, stParser, TextServer.JustificationFlag.WordBound | TextServer.JustificationFlag.Kashida | TextServer.JustificationFlag.SkipLastLine | TextServer.JustificationFlag.DoNotSkipSingleLine);
    }
}

// partial class SurfaceTool
// {
//     /// <inheritdoc cref="AddTriangleFan(Vector3[], Vector2[], Color[], Vector2[], Vector3[], Godot.Collections.Array{Plane})"/>
//     [EditorBrowsable(EditorBrowsableState.Never)]
//     public void AddTriangleFan(Vector3[] vertices, Vector2[] uvs, Color[] colors, Vector2[] uv2S, Vector3[] normals, Godot.Collections.Array tangents)
//     {
//         AddTriangleFan(vertices, uvs, colors, uv2S, normals, new Godot.Collections.Array<Plane>(tangents));
//     }
// }

partial class Tree
{
    /// <inheritdoc cref="EditSelected(bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public bool EditSelected()
    {
        return EditSelected(forceEdit: false);
    }
}

partial class UndoRedo
{
    /// <inheritdoc cref="CreateAction(string, MergeMode, bool)"/>
    [EditorBrowsable(EditorBrowsableState.Never)]
    public void CreateAction(string name, MergeMode mergeMode)
    {
        CreateAction(name, mergeMode, backwardUndoOps: false);
    }
}
