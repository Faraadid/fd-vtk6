/*=========================================================================

  Program:   Visualization Toolkit
  Module:    vtkTextActor3D.cxx

  Copyright (c) Ken Martin, Will Schroeder, Bill Lorensen
  All rights reserved.
  See Copyright.txt or http://www.kitware.com/Copyright.htm for details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE.  See the above copyright notice for more information.

=========================================================================*/
#include "vtkTextActor3D.h"

#include "vtkObjectFactory.h"
#include "vtkCamera.h"
#include "vtkImageActor.h"
#include "vtkImageData.h"
#include "vtkStdString.h"
#include "vtkTransform.h"
#include "vtkTextProperty.h"
#include "vtkTextRenderer.h"
#include "vtkRenderer.h"
#include "vtkRenderWindow.h"
#include "vtkWindow.h"
#include "vtkMatrix4x4.h"
#include "vtkMath.h"

vtkStandardNewMacro(vtkTextActor3D);

vtkCxxSetObjectMacro(vtkTextActor3D, TextProperty, vtkTextProperty);

// ----------------------------------------------------------------------------
vtkTextActor3D::vtkTextActor3D()
{
  this->Input        = NULL;
  this->ImageActor   = vtkImageActor::New();
  this->ImageData    = NULL;
  this->TextProperty = NULL;

  this->BuildTime.Modified();

  this->SetTextProperty(vtkTextProperty::New());
  this->TextProperty->Delete();

  this->ImageActor->InterpolateOn();
}

// --------------------------------------------------------------------------
vtkTextActor3D::~vtkTextActor3D()
{
  this->SetTextProperty(NULL);
  this->SetInput(NULL);

  if (this->ImageActor)
    {
    this->ImageActor->Delete();
    this->ImageActor = NULL;
    }

  if (this->ImageData)
    {
    this->ImageData->Delete();
    this->ImageData = NULL;
    }
}

// --------------------------------------------------------------------------
void vtkTextActor3D::ShallowCopy(vtkProp *prop)
{
  vtkTextActor3D *a = vtkTextActor3D::SafeDownCast(prop);
  if (a != NULL)
    {
    this->SetInput(a->GetInput());
    this->SetTextProperty(a->GetTextProperty());
    }

  this->Superclass::ShallowCopy(prop);
}

// --------------------------------------------------------------------------

double* vtkTextActor3D::GetBounds()
{
  if (this->ImageActor)
    {
    // the culler could be asking our bounds, in which case it's possible
    // that we haven't rendered yet, so we have to make sure our bounds
    // are up to date so that we don't get culled.
    this->UpdateImageActor();
    return this->ImageActor->GetBounds();
    }

  return NULL;
}

// --------------------------------------------------------------------------
int vtkTextActor3D::GetBoundingBox(int bbox[4])
{
  if (!this->TextProperty)
  {
    vtkErrorMacro(<<"Need valid vtkTextProperty.");
    return 0;
  }

  if (!bbox)
  {
    vtkErrorMacro(<<"Need 4-element int array for bounding box.");
  }

  vtkTextRenderer *tRend = vtkTextRenderer::GetInstance();
  if (!tRend)
  {
    vtkErrorMacro(<<"Failed getting the TextRenderer instance.");
    return 0;
  }

  if (!tRend->GetBoundingBox(this->TextProperty, this->Input, bbox))
  {
    vtkErrorMacro(<<"No text in input.");
    return 0;
  }

  return 1;
}

// --------------------------------------------------------------------------
void vtkTextActor3D::ReleaseGraphicsResources(vtkWindow *win)
{
  if (this->ImageActor)
    {
    this->ImageActor->ReleaseGraphicsResources(win);
    }

  this->Superclass::ReleaseGraphicsResources(win);
}

// --------------------------------------------------------------------------
int vtkTextActor3D::RenderOverlay(vtkViewport *viewport)
{
  int rendered_something = 0;

  // Is the viewport's RenderWindow capturing GL2PS-special props?
  if (vtkRenderer *renderer = vtkRenderer::SafeDownCast(viewport))
    {
    if (vtkRenderWindow *renderWindow = renderer->GetRenderWindow())
      {
      if (renderWindow->GetCapturingGL2PSSpecialProps())
        {
        renderer->CaptureGL2PSSpecialProp(this);
        }
      }
    }

  if (this->UpdateImageActor() && this->ImageActor)
    {
    rendered_something += this->ImageActor->RenderOverlay(viewport);
    }

  return rendered_something;
}

// ----------------------------------------------------------------------------
int vtkTextActor3D::RenderTranslucentPolygonalGeometry(vtkViewport *viewport)
{
  int rendered_something = 0;

  if (this->UpdateImageActor() && this->ImageActor)
    {
    rendered_something +=
      this->ImageActor->RenderTranslucentPolygonalGeometry(viewport);
    }

  return rendered_something;
}

//-----------------------------------------------------------------------------
// Description:
// Does this prop have some translucent polygonal geometry?
int vtkTextActor3D::HasTranslucentPolygonalGeometry()
{
  int result = 0;

  if (this->UpdateImageActor() && this->ImageActor)
    {
    result=this->ImageActor->HasTranslucentPolygonalGeometry();
    }

  return result;
}

// --------------------------------------------------------------------------
int vtkTextActor3D::RenderOpaqueGeometry(vtkViewport *viewport)
{
  int rendered_something = 0;

  if (this->UpdateImageActor() && this->ImageActor)
    {
    rendered_something +=
      this->ImageActor->RenderOpaqueGeometry(viewport);
    }

  return rendered_something;
}

// --------------------------------------------------------------------------
int vtkTextActor3D::UpdateImageActor()
{
  // Need text prop
  if (!this->TextProperty)
    {
    vtkErrorMacro(<<"Need a text property to render text actor");
    return 0;
    }

  // No input, the assign the image actor a zilch input
  if (!this->Input || !*this->Input)
    {
    if (this->ImageActor)
      {
      this->ImageActor->SetInputData(0);
      }
    return 1;
    }

  // Do we need to (re-)render the text ?
  // Yes if:
  //  - instance has been modified since last build
  //  - text prop has been modified since last build
  //  - ImageData ivar has not been allocated yet
  if (this->GetMTime() > this->BuildTime ||
      this->TextProperty->GetMTime() > this->BuildTime ||
      !this->ImageData)
    {

    this->BuildTime.Modified();

    // we have to give vtkFTU::RenderString something to work with
    if (!this->ImageData)
      {
      this->ImageData = vtkImageData::New();
      this->ImageData->SetSpacing(1.0, 1.0, 1.0);
      }

    vtkTextRenderer *tRend = vtkTextRenderer::GetInstance();
    if (!tRend)
      {
      vtkErrorMacro(<<"Failed getting the TextRenderer instance.");
      return 0;
      }

    if (!tRend->RenderString(this->TextProperty, this->Input, this->ImageData))
      {
      vtkErrorMacro(<<"Failed rendering text to buffer");
      return 0;
      }

    // Associate the image data (should be up to date now) to the image actor
    if (this->ImageActor)
      {
      this->ImageActor->SetInputData(this->ImageData);
      this->ImageActor->SetDisplayExtent(this->ImageData->GetExtent());
      }

    } // if (this->GetMTime() ...

  // Position the actor
  if (this->ImageActor)
    {
    vtkMatrix4x4 *matrix = this->ImageActor->GetUserMatrix();
    if (!matrix)
      {
      matrix = vtkMatrix4x4::New();
      this->ImageActor->SetUserMatrix(matrix);
      matrix->Delete();
      }
    this->GetMatrix(matrix);
    }

  return 1;
}

// --------------------------------------------------------------------------
void vtkTextActor3D::PrintSelf(ostream& os, vtkIndent indent)
{
  this->Superclass::PrintSelf(os,indent);

  os << indent << "Input: " << (this->Input ? this->Input : "(none)") << "\n";

  if (this->TextProperty)
    {
    os << indent << "Text Property:\n";
    this->TextProperty->PrintSelf(os, indent.GetNextIndent());
    }
  else
    {
    os << indent << "Text Property: (none)\n";
    }
}
