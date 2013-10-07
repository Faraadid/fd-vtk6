vtk_module(vtkIONetCDF
  GROUPS
    StandAlone
  DEPENDS
    vtkCommonDataModel
    vtkCommonSystem
    vtkIOCore
  COMPILE_DEPENDS
    vtknetcdf
  TEST_DEPENDS
    vtkCommonExecutionModel
    vtkRenderingOpenGL
    vtkTestingRendering
    vtkInteractionStyle
  )
